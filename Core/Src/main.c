/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  * @author         : M. Eyub Can, TECH-OHM (https://tech-ohm.com)
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* Phase indexes. High side is a TIM1 channel, low side is a GPIOA pin. */
typedef enum { PHASE_A = 0, PHASE_B, PHASE_C } phase_t;

typedef struct {
  phase_t high;   /* high side, gets the PWM */
  phase_t low;    /* low side, held on for the whole step */
} step_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PWM_STEPS          100U   /* duty resolution, TIM1 ARR = PWM_STEPS - 1 */
#define PWM_PRESCALER      3U     /* 8 MHz / (3 + 1) / 100 = 20 kHz */
#define DUTY_MAX           95U    /* bootstrap drivers cannot hold the high side at 100% duty */

#define THROTTLE_START     150U   /* motor starts above this throttle value (12-bit ADC) */
#define THROTTLE_STOP      100U   /* outputs switch off below this value */
#define THROTTLE_FULL      4095U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

TIM_HandleTypeDef htim1;

/* USER CODE BEGIN PV */
/* Commutation table. Step 0 means everything off. */
static const step_t commutation[7] = {
  { PHASE_A, PHASE_A },   /* 0: not used */
  { PHASE_A, PHASE_B },   /* 1: AH + BL */
  { PHASE_A, PHASE_C },   /* 2: AH + CL */
  { PHASE_B, PHASE_C },   /* 3: BH + CL */
  { PHASE_B, PHASE_A },   /* 4: BH + AL */
  { PHASE_C, PHASE_A },   /* 5: CH + AL */
  { PHASE_C, PHASE_B },   /* 6: CH + BL */
};

/* Hall code (A<<2 | B<<1 | C) -> commutation step in the forward direction.
 * 000 and 111 are invalid. You get them when a sensor wire is broken. */
static const uint8_t hall_to_step[8] = { 0, 6, 4, 5, 2, 1, 3, 0 };

static const uint32_t pwm_channel[3] = { TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3 };
static const uint16_t low_pin[3]     = { GPIO_PIN_5, GPIO_PIN_6, GPIO_PIN_7 };

static volatile uint32_t pwm_duty = 0;   /* 0..DUTY_MAX */
static volatile uint8_t  running  = 0;
static volatile uint8_t  reverse  = 0;   /* 0: forward, 1: reverse */
static uint32_t pedal = 0;               /* filtered ADC value */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM1_Init(void);
/* USER CODE BEGIN PFP */
static void motor_hw_init(void);
static void outputs_off(void);
static void apply_step(uint8_t step, uint32_t duty);
static void commutate(void);
static uint32_t read_pedal(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Settings that override what CubeMX generates. The PWM time base, the hall
 * interrupts and SWD are set here by hand, so regenerating the .ioc keeps them. */
static void motor_hw_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* 20 kHz PWM, duty 0..100 */
  __HAL_TIM_SET_PRESCALER(&htim1, PWM_PRESCALER);
  __HAL_TIM_SET_AUTORELOAD(&htim1, PWM_STEPS - 1U);
  htim1.Instance->EGR = TIM_EGR_UG;

  /* Hall inputs must interrupt on both edges, otherwise 3 of the 6 steps are missed */
  gpio.Pin  = HALL_A_Pin | HALL_B_Pin | HALL_C_Pin;
  gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &gpio);
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  /* JTAG off, SWD stays on so the board can be reprogrammed */
  __HAL_AFIO_REMAP_SWJ_NOJTAG();
}

static void outputs_off(void)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_RESET);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, 0);
}

/* Unused switches go off first, then the switches of the new step come on.
 * No step has the high and low side of the same phase on together. */
static void apply_step(uint8_t step, uint32_t duty)
{
  if (step < 1U || step > 6U) {
    outputs_off();
    return;
  }

  const step_t *s = &commutation[step];

  for (uint8_t p = 0; p < 3U; p++) {
    if (p != s->low)  HAL_GPIO_WritePin(GPIOA, low_pin[p], GPIO_PIN_RESET);
    if (p != s->high) __HAL_TIM_SET_COMPARE(&htim1, pwm_channel[p], 0);
  }
  HAL_GPIO_WritePin(GPIOA, low_pin[s->low], GPIO_PIN_SET);
  __HAL_TIM_SET_COMPARE(&htim1, pwm_channel[s->high], duty);
}

/* Reads the hall sensors and applies the step that matches the rotor position.
 * It works at standstill too, so no separate open-loop start is needed. */
static void commutate(void)
{
  uint8_t hall = 0;

  if (HAL_GPIO_ReadPin(GPIOB, HALL_A_Pin) == GPIO_PIN_SET) hall |= 0x4U;
  if (HAL_GPIO_ReadPin(GPIOB, HALL_B_Pin) == GPIO_PIN_SET) hall |= 0x2U;
  if (HAL_GPIO_ReadPin(GPIOB, HALL_C_Pin) == GPIO_PIN_SET) hall |= 0x1U;

  uint8_t step = hall_to_step[hall];

  /* In reverse the same hall code selects the step 180 degrees away */
  if (step != 0U && reverse) {
    step = (uint8_t)(((step + 2U) % 6U) + 1U);
  }

  apply_step(running ? step : 0U, pwm_duty);
}

/* Throttle potentiometer. A single sample is noisy, hence the 1/8 IIR filter. */
static uint32_t read_pedal(void)
{
  HAL_ADC_Start(&hadc1);
  if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK) {
    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    pedal = (pedal * 7U + raw) / 8U;
  }
  return pedal;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin & (HALL_A_Pin | HALL_B_Pin | HALL_C_Pin)) {
    commutate();
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
  MX_ADC1_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  motor_hw_init();
  outputs_off();
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t throttle = read_pedal();

    if (!running && throttle >= THROTTLE_START) {
      /* Direction is read only at standstill. It cannot change while the motor turns. */
      reverse = (HAL_GPIO_ReadPin(vites_GPIO_Port, vites_Pin) == GPIO_PIN_SET);
      running = 1;
    } else if (running && throttle < THROTTLE_STOP) {
      running = 0;
    }

    if (running) {
      pwm_duty = (throttle - THROTTLE_STOP) * DUTY_MAX / (THROTTLE_FULL - THROTTLE_STOP);
    } else {
      pwm_duty = 0;
    }

    /* Commutation also runs from the main loop, to update the duty and to retry
     * if the motor stalls. The hall interrupt is masked so the two cannot collide. */
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    commutate();
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    HAL_Delay(1);
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 65535;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin : vites_Pin */
  GPIO_InitStruct.Pin = vites_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(vites_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PA5 PA6 PA7 */
  GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : HALL_A_Pin HALL_B_Pin HALL_C_Pin */
  GPIO_InitStruct.Pin = HALL_A_Pin|HALL_B_Pin|HALL_C_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* The hall pins have no NVIC interrupt enabled in CubeMX, so the handler lives here */
void EXTI9_5_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(HALL_A_Pin);
  HAL_GPIO_EXTI_IRQHandler(HALL_B_Pin);
  HAL_GPIO_EXTI_IRQHandler(HALL_C_Pin);
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

#ifdef  USE_FULL_ASSERT
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
