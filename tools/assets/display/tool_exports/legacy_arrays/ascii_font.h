#ifndef ASCII_FONT_H
#define ASCII_FONT_H
#include "config.h"

#if defined(__STM32__HAL__)
#include "stm32f1xx_hal.h"
#elif defined(__SC95F8615__)
#include "H/Function_Init.H"
#elif defined(__N32G031__)
#include "main.h"
#endif

extern const uint8_t ascii_font[95][16];      // 16x16 字体
extern const uint8_t ascii_font5x10[95][10];  // 5x10 字体
extern const uint8_t ascii_font6x12[95][12];  // 6x12 字体
extern const uint8_t dir_Image[2][128];       // 方向图标
extern const uint8_t dir_CCW[128];            // 逆时针图标
extern const uint8_t TorqueImage[2][128];     // 转矩图标
extern const uint8_t BattayImage[5][46];      // 电池图标
extern const uint8_t ChargeImage[5][256];     // 充电图标
extern const uint8_t CharConnectImage[256];   // 字符连接图标
extern const uint8_t ModeImage[26];           // 模式图标
extern const uint8_t ModeNum[11][20];         // 模式数字
extern const uint8_t SpeedUnit[64];           // 速度单位
extern const uint8_t TorqueUnit[64];          // 转矩单位
extern const uint8_t AngleUnit[32];           // 角度单位
extern const uint8_t StartupImage[300];       // 启动图标
extern const uint8_t SetSpeedTQR[72];         // 速度转矩角度图标
extern const uint8_t SetTorqueRD[72];         // 转矩角度图标
/*-----------------------------------------------------------*/
extern const uint8_t SetActionTQR[72];
extern const uint8_t SetStartTQR[72];
extern const uint8_t SetStopTQR[72];
extern const uint8_t SetSlowTQR[72];
extern const uint8_t SetPointTQR[72];
extern const uint8_t StopImage[80];
extern const uint8_t OffImage[80];
extern const uint8_t OnImage[80];
extern const uint8_t RevImage[80];
/*-----------------------------------------------------------*/
extern const uint8_t SetDirMEM[72];           // 方向图标
extern const uint8_t AddSubImage[124];        // 加减图标
extern const uint8_t ADDImage[124];           // 加图标
extern const uint8_t SUBImage[124];           // 减图标
extern const uint8_t SetDirFwd[124];          // 正方向图标
extern const uint8_t SetDirRev[124];          // 反方向图标
extern const uint8_t SetDIRImage[2][124];     // 方向图标
extern const uint8_t ProgressBar[512];        // 进度条图标
extern const uint8_t BatteyImageCol[5][33];   // 电池图标
extern const uint8_t ascii_font8x16[][16];    // 8x16 字体
extern const uint8_t ASCII_FONT8x16[95][16];  // 8x16 字体
extern const uint8_t ASCII_FONT12x24[95][36]; // 12x24 字体
extern const uint8_t ProgressCor[48];         // 进度条图标
extern const uint8_t ForwordRev[72];          // 前进后退图标
extern const uint8_t ModeFre[72];             // 功能频率图标
/// @brief 自动工作模式类型图标
extern const uint8_t AutoImage[128];
/// @brief 速度设置指示图标
extern const uint8_t SPImage[72];
/// @brief 角度设置指示图标
extern const uint8_t ANGImage[72];
#endif
