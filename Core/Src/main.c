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
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
#define MP3_RX_MAX_LEN 15  // 稍微定义大一点防止溢出
uint8_t mp3_rx_buffer[MP3_RX_MAX_LEN];  // DMA 接收大本营
volatile uint8_t mp3_rx_flag = 0;       // 接收完成标志位
volatile uint8_t mp3_rx_len = 0;        // 实际接收到的长度
uint32_t mp3_last_rx_tick = 0;          // 上次收到数据的时间戳
uint8_t  mp3_init_ok = 0;              // 模块初始化完成标志
uint8_t  mp3_need_play = 0;            // 需要发播放指令的标志
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// ==================== YX080 MP3-TF-16P 协议函数 ====================
// 手册: 帧格式 $S VER Len CMD FB para1 para2 checksum $O
//       起始 7E | 版本 FF | 长度 06 | 命令 | 反馈 | 参数H | 参数L | 校验2B | 结束 EF

// 计算校验和 (累加和取反，不计起始位$)
static uint16_t MP3_CalcChecksum(uint8_t *data, uint8_t len)
{
    uint16_t sum = 0;
    for(uint8_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(-sum);
}

// 发送指令到 MP3-TF-16P 模块 (纯发送, 不动DMA)
static void MP3_SendCmd(uint8_t cmd, uint8_t feedback, uint16_t param)
{
    uint8_t buf[10];
    uint16_t cs;
    buf[0] = 0x7E;
    buf[1] = 0xFF;
    buf[2] = 0x06;
    buf[3] = cmd;
    buf[4] = feedback;
    buf[5] = (param >> 8) & 0xFF;
    buf[6] = param & 0xFF;
    cs = MP3_CalcChecksum(&buf[1], 6);
    buf[7] = (cs >> 8) & 0xFF;
    buf[8] = cs & 0xFF;
    buf[9] = 0xEF;
    HAL_UART_Transmit(&huart1, buf, 10, HAL_MAX_DELAY);
}

// 解析收到的数据包类型
static void MP3_ParsePacket(uint8_t *buf, uint8_t len)
{
    if(len < 10 || buf[0] != 0x7E || buf[9] != 0xEF)
    {
        printf("  [WARN] Invalid packet format!\r\n");
        return;
    }
    uint8_t cmd = buf[3];
    switch(cmd)
    {
        case 0x3F: printf("  -> Module Init: device=0x%02X\r\n", buf[6]); break;
        case 0x3A: printf("  -> Device Inserted: type=0x%02X\r\n", buf[6]); break;
        case 0x3B: printf("  -> Device Removed: type=0x%02X\r\n", buf[6]); break;
        case 0x3C: printf("  -> USB track #%d finished\r\n", buf[6]); break;
        case 0x3D: printf("  -> TF track #%d finished\r\n", buf[6]); break;
        case 0x3E: printf("  -> FLASH track #%d finished\r\n", buf[6]); break;
        case 0x40: printf("  -> ERROR: code=0x%02X\r\n", buf[6]); break;
        case 0x41: printf("  -> ACK: cmd=0x%02X OK\r\n", buf[6]); break;
        case 0x42: printf("  -> Status: 0x%02X\r\n", buf[6]); break;
        case 0x48: printf("  -> TF卡曲目总数 = %d\r\n", buf[6]); break;
        default:   printf("  -> CMD=0x%02X param=0x%02X\r\n", cmd, buf[6]); break;
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
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_FSMC_Init();
  /* USER CODE BEGIN 2 */
  printf("========================================\r\n");
  printf("  ZET6 MP3 Player (YX080 MP3-TF-16P)\r\n");
  printf("========================================\r\n");
  printf("USART2 -> printf debug (115200)\r\n");
  printf("USART1 -> MP3 module (9600)\r\n");
  printf("Waiting for module init msg...\r\n");
  printf("(Make sure TF card is inserted!)\r\n");
  printf("========================================\r\n");

  // 1. 开启 USART1 的空闲中断 (IDLE)
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);

  // 2. 启动 DMA 接收
  HAL_UART_Receive_DMA(&huart1, mp3_rx_buffer, MP3_RX_MAX_LEN);

  // 3. 记录启动时间
  mp3_last_rx_tick = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      // ========== A. 处理收到的数据 ==========
      if(mp3_rx_flag == 1)
      {
          if(mp3_rx_len == 0)
          {
              // 0字节是干扰, 跳过, 静默重启DMA
              mp3_rx_flag = 0;
              HAL_UART_Receive_DMA(&huart1, mp3_rx_buffer, MP3_RX_MAX_LEN);
          }
          else
          {
              printf("[RX] %d字节: ", mp3_rx_len);
              for(int i = 0; i < mp3_rx_len; i++)
                  printf("%02X ", mp3_rx_buffer[i]);
              printf("\r\n");

              MP3_ParsePacket(mp3_rx_buffer, mp3_rx_len);

              // 首次收到合法应答 → 模块通信OK
              if(mp3_init_ok == 0 && mp3_rx_len >= 10
                 && mp3_rx_buffer[0] == 0x7E && mp3_rx_buffer[9] == 0xEF)
              {
                  mp3_init_ok = 1;
                  mp3_need_play = 1;  // 通知外面发播放指令
                  printf("========================================\r\n");
                  printf("[OK] MP3模块通信成功!\r\n");
                  printf("========================================\r\n");
              }

              mp3_rx_flag = 0;
              mp3_last_rx_tick = HAL_GetTick();
              HAL_UART_Receive_DMA(&huart1, mp3_rx_buffer, MP3_RX_MAX_LEN);
          }
      }

      // ========== B. 正式播放音乐 ==========
      if(mp3_need_play == 1)
      {
          mp3_need_play = 0;

          printf("\r\n========================================\r\n");
          printf("  开始播放流程\r\n");

          printf("[CMD] 1. 指定TF卡设备...\r\n");
          MP3_SendCmd(0x09, 0x00, 0x0002);
          HAL_Delay(1000);

          printf("[CMD] 2. 设置音量=25...\r\n");
          MP3_SendCmd(0x06, 0x00, 20);
          HAL_Delay(100);

          printf("[CMD] 3. 播放第1首!\r\n");
          MP3_SendCmd(0x03, 0x00, 0x0001);

          printf("========================================\r\n");
          printf("[INFO] 音乐应该响起了!\r\n");
          printf("========================================\r\n");

          // 清理接收通道
          HAL_UART_AbortReceive(&huart1);
          __HAL_UART_CLEAR_OREFLAG(&huart1);
          __HAL_UART_CLEAR_IDLEFLAG(&huart1);
          mp3_rx_flag = 0;
          HAL_UART_Receive_DMA(&huart1, mp3_rx_buffer, MP3_RX_MAX_LEN);
      }

      // ========== C. 超时探测 (3秒无数据) ==========
      if(mp3_init_ok == 0 && (HAL_GetTick() - mp3_last_rx_tick > 3000))
      {
          printf("\r\n[!!!] MP3模块3秒无应答! 检查: TF卡? 接线?\r\n");
          printf("[探测] 发送状态查询...\r\n");
          MP3_SendCmd(0x42, 0x00, 0x0000);
          mp3_last_rx_tick = HAL_GetTick();
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
}

/* USER CODE BEGIN 4 */
// 适配 Keil (MDK-ARM) + MicroLib
int fputc(int ch, FILE *f)
{
    // 将 printf 的单个字符通过 USART2 轮询发送
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

// 适配 GCC / STM32CubeIDE
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
