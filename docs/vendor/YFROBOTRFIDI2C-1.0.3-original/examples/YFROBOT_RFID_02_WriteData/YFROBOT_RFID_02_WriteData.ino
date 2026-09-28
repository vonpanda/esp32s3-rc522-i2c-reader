/*
 * YFROBOT RFID IIC 通讯示例：写入卡数据
 * 
 * 本示例程序通过 IIC 通讯接口将数据写入 RFID 卡的指定数据块。
 * 程序首先检测是否有新的卡片进入读卡范围，然后选择卡片并写入数据。
 * 写入完成后，程序会打印写入结果到串口监视器。
 * 
 * 作者: YFROBOTZL
 * 日期: 2024-11-17
 * 链接: http://www.yfrobot.com.cn
 */

#include <Wire.h>
#include "YFROBOTRFIDI2C.h"
YFROBOTRFID yf_rfid;
YFROBOTRFID::StatusCode status;

byte block = 4;  // 写入卡中的数据块位置（0~63，其中有部分不可写入，详情请查看资料）
// 要写入的 字符串 yfrobot.com.cn  ，16进制数组，数据中 0x20 为补位空格
byte data[] = { 0x79, 0x66, 0x72, 0x6F,
                 0x62, 0x6F, 0x74, 0x2E,
                 0x63, 0x6f, 0x6d, 0x2E,
                 0x63, 0x6E, 0x20, 0x20 };

void setup() {
  Serial.begin(9600);   // Initialize serial communications with the PC
  Wire.begin();         // Initialize I2C
  yf_rfid.PCD_Init();   // Init YFROBOT RFID
  String ver = yf_rfid.GetReaderVersion();
  Serial.println(ver);  // YFROBOT RFID Card Reader ver
  Serial.println(F("Write personal data on a MIFARE PICC"));
}

void loop() {
  // Look for new cards, and select one if present
  if (!yf_rfid.PICC_IsNewCardPresent() || !yf_rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  // 将数据写入指定的数据块
  writeBytesToBlock(block, data);

  // Dump debug info about the card; PICC_HaltA() is automatically called
  // 打印卡相关调试信息；自动调用PICC_HaltA()停止重复读卡
  yf_rfid.PICC_DumpToSerial(&(yf_rfid.uid));

  yf_rfid.PICC_HaltA();       // 指示处于 ACTIVE(*) 状态的 PICC 进入 HALT 状态。
  yf_rfid.PCD_StopCrypto1();  // 用于使 PCD 从其已验证状态退出。
}

/**
 * 将数据写入YFROBOT RFID卡的指定块。
 * 
 * 此函数首先尝试使用指定的密钥对RFID卡上的块进行身份验证，
 * 如果成功，将继续从缓冲区写入数据到该块。
 * 
 * @param block 要写入的块号。
 * @param buff 包含要写入的数据的缓冲区，必须至少包含16个字节。
 */
void writeBytesToBlock(byte block, byte buff[]) {
  // 尝试使用Key A对块进行身份验证
  status = (YFROBOTRFID::StatusCode)yf_rfid.PCD_Authenticate(YFROBOTRFID::PICC_CMD_MF_AUTH_KEY_A, block, &yf_rfid.keyA, &(yf_rfid.uid));

  if (status != YFROBOTRFID::STATUS_OK) {  // 检查身份验证状态
    Serial.print(F("PCD_Authenticate() failed: "));
    Serial.println(yf_rfid.GetStatusCodeName(status));
    return;
  } else Serial.println(F("PCD_Authenticate() success: "));
  // 写入块
  status = (YFROBOTRFID::StatusCode)yf_rfid.MIFARE_Write(block, buff, 16);

  if (status != YFROBOTRFID::STATUS_OK) {  // 检查写入状态
    Serial.print(F("MIFARE_Write() failed: "));
    Serial.println(yf_rfid.GetStatusCodeName(status));
    return;
  } else Serial.println(F("MIFARE_Write() success: "));
}
