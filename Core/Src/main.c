/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "fatfs.h"
#include "i2s.h"
#include "sdio.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "fatfs.h"
#include "mp3dec.h"    // 引入你的骄傲：Helix 解码库头文件
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ERR_MP3_NONE           0
#define ERR_MP3_INDATA_UNDERFLOW 1
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// =========================================================================
// 全局内存规划 (极其重要：千万不要放在局部变量里，防止栈溢出！)
// =========================================================================
FATFS fs;              /* FatFS file-system work area */
FIL mp3File;           /* MP3 file handle */

// 1. MP3 文件读取缓冲区 (从 SD 卡一次读入这么多数据等待解析)
#define READBUF_SIZE 8192       // Helix 推荐的安全大小
uint8_t readBuf[READBUF_SIZE];

// 2. I2S DMA 乒乓缓冲区 (双缓冲结构)
// 假设一帧 MP3 最多解出 1152*2(双声道) = 2304 个采样点 (short类型,占4608字节)
// 我们开辟两帧的大小作为双缓冲: 2304 * 2 = 4608 个 short = 9216 字节
#define I2S_DMA_BUF_LEN (2304 * 2)
int16_t i2sDmaBuf[I2S_DMA_BUF_LEN];

// 3. Helix 解码器句柄与信息结构体
HMP3Decoder mp3Decoder;
MP3FrameInfo mp3Info;

// 4. 乒乓缓冲状态标志
volatile uint8_t dmaHalfCplt = 0; // 前半段播完标志 (Ping)
volatile uint8_t dmaFullCplt = 0; // 后半段播完标志 (Pong)
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
extern void Convert_Stereo(short *buffer, int nSamps);
extern void Convert_Mono(short *buffer, int nSamps);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

uint8_t BSP_SD_IsDetected(void)
{
  return SD_PRESENT;
}


/* ========================================================================= */
/* ==================== 1. 添加这两个回调函数 (必须放在 main 函数外面) === */
/* ========================================================================= */

// DMA 传输过半中断 (Ping)
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if(hi2s->Instance == SPI2) // 确认是 I2S2 触发的
    {
        dmaHalfCplt = 1; // 通知主循环：赶紧填前半段！
    }
}

// DMA 传输完成中断 (Pong)
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if(hi2s->Instance == SPI2) // 确认是 I2S2 触发的
    {
        dmaFullCplt = 1; // 通知主循环：赶紧填后半段！
    }
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_FSMC_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
  MX_I2S2_Init();
  /* USER CODE BEGIN 2 */

    // 1. 统一全局生命周期的局部变量 (去掉重复的 br)
    UINT br;
    int bytesLeft = 0;
    unsigned char *readPtr;
    int offset = 0;  // 解决 offset undefined
    int err = 0;     // 解决 err undefined

    printf("========================================\r\n");
    printf("ZET6 硬核音频架构: Helix MP3 解码 + I2S 双缓冲测试 (工业级修复版)\r\n");
    printf("========================================\r\n");

    if(f_mount(&fs, "0:/", 1) == FR_OK) 
    {
        printf("[OK] SD卡挂载成功！\r\n");
        
        if(f_open(&mp3File, "0:/2.mp3", FA_READ) == FR_OK) 
        {
            printf("[OK] 成功打开 2.mp3，准备解码...\r\n");
            
            mp3Decoder = MP3InitDecoder();
            if(mp3Decoder == 0) {
                printf("[ERR] 内存不足！Helix 解码器初始化失败。\r\n");
                while(1);
            }
            printf("[OK] Helix 初始化成功！\r\n");
            
            // 第一次装填弹药库
            f_read(&mp3File, readBuf, READBUF_SIZE, &br);
            bytesLeft = br;
            readPtr = readBuf;
            
            // =========================================================
            // 预充填 Buffer A (第1帧)
            // =========================================================
            offset = MP3FindSyncWord(readPtr, bytesLeft);
            if(offset >= 0) {
                readPtr += offset; bytesLeft -= offset;
                err = MP3Decode(mp3Decoder, &readPtr, &bytesLeft, i2sDmaBuf, 0);
                if(err == ERR_MP3_NONE) {
                    MP3GetLastFrameInfo(mp3Decoder, &mp3Info);
                    printf("\r\n--- 歌曲情报 ---\r\n采样率: %d Hz, 声道: %d, 输出采样: %d\r\n",
                           mp3Info.samprate, mp3Info.nChans, mp3Info.outputSamps);

                    // 根据声道数调用对应的转换函数 (Helix Subband 输出必须转换!)
                    if (mp3Info.nChans == 2) {
                        Convert_Stereo(i2sDmaBuf, mp3Info.outputSamps);
                    } else if (mp3Info.nChans == 1) {
                        Convert_Mono(i2sDmaBuf, mp3Info.outputSamps);
                    }

                    // 动态同步 I2S 硬件时钟, 匹配当前歌曲采样率
                    HAL_I2S_DeInit(&hi2s2);
                    hi2s2.Init.AudioFreq = mp3Info.samprate;
                    if(HAL_I2S_Init(&hi2s2) == HAL_OK) {
                        printf("[OK] I2S 硬件时钟已同步至: %d Hz\r\n", mp3Info.samprate);
                    } else {
                        printf("[WARN] I2S 时钟同步失败!\r\n");
                    }
                }
            }

            // =========================================================
            // 预充填 Buffer B (第2帧)
            // =========================================================
            if (bytesLeft < 4096) {
                memmove(readBuf, readPtr, bytesLeft);
                f_read(&mp3File, readBuf + bytesLeft, READBUF_SIZE - bytesLeft, &br);
                bytesLeft += br;
                readPtr = readBuf;
            }

            offset = MP3FindSyncWord(readPtr, bytesLeft);
            if (offset >= 0) {
                readPtr += offset; bytesLeft -= offset;
                err = MP3Decode(mp3Decoder, &readPtr, &bytesLeft, i2sDmaBuf + 2304, 0);
                if(err == ERR_MP3_NONE) {
                    MP3GetLastFrameInfo(mp3Decoder, &mp3Info);
                    if (mp3Info.nChans == 2) {
                        Convert_Stereo(i2sDmaBuf + 2304, mp3Info.outputSamps);
                    } else if (mp3Info.nChans == 1) {
                        Convert_Mono(i2sDmaBuf + 2304, mp3Info.outputSamps);
                    }
                }
            }

            printf("[OK] 预充填完毕，开闸!\r\n");
            HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t*)i2sDmaBuf, 4608);
        }
        else 
        {
            printf("[ERR] 打开 2.mp3 失败！\r\n");
        }
    }
    else 
    {
        printf("[ERR] SD 卡挂载失败！\r\n");
    }
    
    
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      
      // =================================================================
      // 核心供水站：时刻保持弹药库充足！
      // =================================================================
      if (bytesLeft < 2048) 
      {
          // 1. 把剩下的残余数据挪到缓冲区最前面
          memmove(readBuf, readPtr, bytesLeft); 
          
          // 2. 从 SD 卡读取新数据填满缓冲区
          f_read(&mp3File, readBuf + bytesLeft, READBUF_SIZE - bytesLeft, &br);
          
          if (br == 0 && bytesLeft == 0) {
               printf("[INFO] 歌曲播放结束！\r\n");
               HAL_I2S_DMAStop(&hi2s2);
               break; 
          }
          
          // 3. 指针复位，缓冲区满
          bytesLeft += br;
          readPtr = readBuf;
      }

      // =================================================================
      // DMA 回调处理: 两个标志可能同时置位(若 f_read 耗时过长),
      // 必须在本轮循环内全部处理, 否则 DMA 重放旧数据 -> 噪音
      // =================================================================

      // 填补 Buffer A (前半段)
      if (dmaHalfCplt)
      {
          dmaHalfCplt = 0;

          offset = MP3FindSyncWord(readPtr, bytesLeft);
          if (offset >= 0) {
              readPtr += offset; bytesLeft -= offset;
              err = MP3Decode(mp3Decoder, &readPtr, &bytesLeft, i2sDmaBuf, 0);
              if(err == ERR_MP3_NONE) {
                  MP3GetLastFrameInfo(mp3Decoder, &mp3Info);
                  if (mp3Info.nChans == 2) {
                      Convert_Stereo(i2sDmaBuf, mp3Info.outputSamps);
                  } else if (mp3Info.nChans == 1) {
                      Convert_Mono(i2sDmaBuf, mp3Info.outputSamps);
                  }
              } else {
                  /* 解码失败, 零填充防噪音 */
                  memset(i2sDmaBuf, 0, 2304 * sizeof(int16_t));
              }
          } else {
              /* 未找到同步字, 零填充 */
              memset(i2sDmaBuf, 0, 2304 * sizeof(int16_t));
          }
      }

      // 填补 Buffer B (后半段)
      if (dmaFullCplt)
      {
          dmaFullCplt = 0;

          offset = MP3FindSyncWord(readPtr, bytesLeft);
          if (offset >= 0) {
              readPtr += offset; bytesLeft -= offset;
              err = MP3Decode(mp3Decoder, &readPtr, &bytesLeft, i2sDmaBuf + 2304, 0);
              if(err == ERR_MP3_NONE) {
                  MP3GetLastFrameInfo(mp3Decoder, &mp3Info);
                  if (mp3Info.nChans == 2) {
                      Convert_Stereo(i2sDmaBuf + 2304, mp3Info.outputSamps);
                  } else if (mp3Info.nChans == 1) {
                      Convert_Mono(i2sDmaBuf + 2304, mp3Info.outputSamps);
                  }
              } else {
                  memset(i2sDmaBuf + 2304, 0, 2304 * sizeof(int16_t));
              }
          } else {
              memset(i2sDmaBuf + 2304, 0, 2304 * sizeof(int16_t));
          }
      }
      
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_I2S2;
  PeriphClkInit.I2s2ClockSelection = RCC_I2S2CLKSOURCE_SYSCLK;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* printf redirect to USART2 (Keil MDK-ARM / MicroLib) */
int fputc(int ch, FILE *f)
{
    (void)f;
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

/* printf redirect to USART2 (GCC / STM32CubeIDE) */
int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
