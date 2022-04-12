
#include "AS608.h"

AS_608 g_as608;
int   g_fd;          // file char, return when serial opened
int   g_verbose;     // 输出信息的详细程度
char  g_error_desc[128]; // 错误代码的含义
uchar g_error_code;      // 模块返回的确认码，如果函数返回值不为true，读取此变量

uchar g_order[64] = { 0 }; // 发送给模块的指令包
uchar g_reply[64] = { 0 }; // 模块的应答包 


/******************************************************************************
 *
 * 第一部分：辅助函数区
 *   该部分的函数仅在本文件作用域内有效，在 as608.h 中没有声明！！！
 *
******************************************************************************/


void Car::Split(uint num, uchar* buf, int count) {
  for (int i = 0; i < count; ++i) {
    *buf++ = (num & 0xff << 8*(count-i-1)) >> 8*(count-i-1);
  }
}


bool Car::Merge(uint* num, const uchar* startAddr, int count) {
  *num = 0;
  for (int i = 0; i < count; ++i)
    *num += (int)(startAddr[i]) << (8*(count-i-1)); 

  return true;
}


void Car::PrintBuf(const uchar* buf, int size) {
  for (int i = 0; i < size; ++i) {
    printf("%02X ", buf[i]);
  }
  printf("\n");
}


int Car::Calibrate(const uchar* buf, int size) {
  int count = 0;
  for (int i = 6; i < size - 2; ++i) {
    count += buf[i];
  }

  return count;
}


bool Car::Check(const uchar* buf, int size) {
  int count_ = 0;       // 模块传来的检校和 
  Merge(&count_, buf+size-2, 2); 

  // 自己计算的检校和
  int count = Calibrate(buf, size);

  return (buf[9] == 0x00 && count_ == count && buf[0] != 0x00);   // 防止全为0x00
}


int Car::SendOrder(const uchar* order, int size) {
  // 输出详细信息
  if (g_verbose == 1) {
    printf("sent: ");
    PrintBuf(order, size);
  }
  int ret = write(g_fd, order, size);
  return ret;
}


bool Car::RecvReply(uchar* hex, int size) {
  int availCount = 0;
  int timeCount  = 0;

  while (true) {
    if (serialDataAvail(g_fd)) {
      hex[availCount] = serialGetchar(g_fd);
      availCount++;
      if (availCount >= size) {
        break;
      }
    }
    usleep(10); // 等待10微秒
    timeCount++;
    if (timeCount > 300000) {   // 最长阻塞3秒
      break;
    }
  }

  // 输出详细信息
  if (g_verbose == 1) {
    printf("recv: ");
    PrintBuf(hex, availCount);
  }

  // 最大阻塞时间内未接受到指定大小的数据，返回false
  if (availCount < size) {
    g_error_code = 0xff;
    return false;
  }

  g_error_code = hex[9];
  return true;
}


void Car::PrintProcess(int done, int all) {
  // 进度条，0~100%，50个字符
  char process[64] = { 0 };
  double stepSize = (double)all / 100;
  int doneStep = done / stepSize;
  
  // 如果步数是偶数，更新进度条
  // 因为用50个字符显示100个进度值
  for (int i = 0; i < doneStep / 2 - 1; ++i)
    process[i] = '=';
  process[doneStep/2 - 1] = '>';

  printf("\rProcess:[%-50s]%d%% ", process, doneStep);
  if (done < all)
    fflush(stdout);
  else
    printf("\n");
}


bool Car::RecvPacket(uchar* pData, int validDataSize) {
  if (g_as608.packet_size <= 0)
    return false;
  int realPacketSize = 11 + g_as608.packet_size; // 实际每个数据包的大小
  int realDataSize = validDataSize * realPacketSize / g_as608.packet_size;  // 总共需要接受的数据大小

  uchar readBufTmp[8] = { 0 };  // 每次read至多8个字节，追加到readBuf中
  uchar* readBuf = (uchar*)malloc(realPacketSize); // 收满realPacketSize字节，说明收到了一个完整的数据包，追加到pData中

  int availSize      = 0;
  int readSize       = 0;
  int readCount      = 0;
  //unused variable
  //int readBufTmpSize = 0;
  int readBufSize    = 0;
  int offset         = 0;
  int timeCount      = 0;
  
  while (true) {
    if ((availSize = serialDataAvail(g_fd)) > 0) {
      timeCount = 0;
      if (availSize > 8) {
        availSize = 8;
      }
      if (readBufSize + availSize > realPacketSize) {
        availSize = realPacketSize - readBufSize;
      }

      memset(readBufTmp, 0, 8);
      readSize = read(g_fd, readBufTmp, availSize);
      memcpy(readBuf+readBufSize, readBufTmp, readSize);

      readBufSize += readSize;
      readCount   += readSize;

      // 是否输出详细信息
      if (g_verbose == 1) {
        printf("%2d%% RecvData: %d  count=%4d/%-4d  ", 
            (int)((double)readCount/realDataSize*100), readSize, readCount, realDataSize);
        PrintBuf(readBufTmp, readSize);
      }
      else if (g_verbose == 0){ // 默认显示进度条
        PrintProcess(readCount, realDataSize);
      }
      else {
        // show nothing
      }

      // 接收完一个完整的数据包(139 bytes)
      if (readBufSize >= realPacketSize) {
        int count_ = 0;
        Merge(&count_, readBuf+realPacketSize-2, 2);
        if (Calibrate(readBuf, realPacketSize) != count_) {
          free(readBuf);
          g_error_code = 0x01;
          return false;
        }

        memcpy(pData+offset, readBuf+9, g_as608.packet_size);
        offset += g_as608.packet_size;
        readBufSize = 0;

        // 收到 结束包
        if (readBuf[6] == 0x08) {
          break;
        }
      }

      // 接受到 validDataSize 个字节的有效数据，但仍未收到结束包，
      if (readCount >= realDataSize) {
        free(readBuf);
        g_error_code = 0xC4;
        return false;
      }
    } // end outer if

    usleep(10); // 等待10微秒
    timeCount++;
    if (timeCount > 300000) {   // 最长阻塞3秒
      break;
    }
  } // end while

  free(readBuf);

  // 最大阻塞时间内未接受到指定大小的数据，返回false
  if (readCount < realDataSize) {
    g_error_code = 0xC3;
    return false;
  }
  
  g_error_code = 0x00;
  return true; 
}



bool Car::SendPacket(uchar* pData, int validDataSize) {
  if (g_as608.packet_size <= 0)
    return false;
  if (validDataSize % g_as608.packet_size != 0) {
    g_error_code = 0xC8;
    return false;
  }
  int realPacketSize = 11 + g_as608.packet_size; // 实际每个数据包的大小
  int realDataSize = validDataSize * realPacketSize / g_as608.packet_size;  // 总共需要发送的数据大小

  // 构造数据包
  uchar* writeBuf = (uchar*)malloc(realPacketSize);
  writeBuf[0] = 0xef;  // 包头
  writeBuf[1] = 0x01;  // 包头
  Split(g_as608.chip_addr, writeBuf+2, 4);  // 芯片地址
  Split(g_as608.packet_size+2, writeBuf+7, 2);  // 包长度

  int offset     = 0;  // 已发送的有效数据
  int writeCount = 0;  // 已发送的实际数据

  while (true) {
    // 填充数据区域
    memcpy(writeBuf+9, pData+offset, g_as608.packet_size);

    // 数据包 标志
    if (offset + g_as608.packet_size < validDataSize)
      writeBuf[6] = 0x02;  // 结束包(最后一个数据包)
    else
      writeBuf[6] = 0x08;  // 普通数据包

    // 检校和
    Split(Calibrate(writeBuf, realPacketSize), writeBuf+realPacketSize-2, 2); 

    // 发送数据包
    write(g_fd, writeBuf, realPacketSize);

    offset     += g_as608.packet_size;
    writeCount += realPacketSize;

    // 是否输出详细信息
    if (g_verbose == 1) {
      printf("%2d%% SentData: %d  count=%4d/%-4d  ", 
          (int)((double)writeCount/realDataSize*100), realPacketSize, writeCount, realDataSize);
      PrintBuf(writeBuf, realPacketSize);
    }
    else if (g_verbose == 0) {
      // 显示进度条
      PrintProcess(writeCount, realDataSize);
    }
    else {
      // show nothing
    }

    if (offset >= validDataSize)
      break;
  } // end while

  free(writeBuf);
  g_error_code = 0x00;
  return true; 
}

int Car::GenOrder(uchar orderCode, const char* fmt, ...) {
  g_order[0] = 0xef;        // 包头，0xef
  g_order[1] = 0x01;
  Split(g_as608.chip_addr, g_order+2, 4);    // 芯片地址，需要使用PS_Setup()初始化设置
  g_order[6] = 0x01;        // 包标识，0x01代表是指令包，0x02数据包，0x08结束包(最后一个数据包)
  g_order[9] = orderCode;   // 指令

  // 计算 参数总个数
  int count = 0;
  for (const char* p = fmt; *p; ++p) {
    if (*p == '%')
      count++;
  }

  // fmt==""
  if (count == 0) { 
    Split(0x03, g_order+7,  2);  // 包长度
    Split(Calibrate(g_order, 0x0c), g_order+10, 2);  // 检校和(如果不带参数，指令包长度为12，即0x0c)
    return 0x0c;
  }
  else {
    va_list ap;
    va_start(ap, fmt);

    uint  uintVal;
    uchar ucharVal;
    uchar* strVal;

    int offset = 10;  // g_order指针偏移量
    int width = 1;    // fmt中修饰符的宽度，如%4d, %32s

    // 处理不定参数
    for (; *fmt; ++fmt) {
      width = 1;
      if (*fmt == '%') {
        const char* tmp = fmt+1;

        // 获取宽度，如 %4u, %32s
        if (*tmp >= '0' && *tmp <= '9') {
          width = 0;
          do {
            width = (*tmp - '0') + width * 10;
            tmp++;
          } while(*tmp >= '0' && *tmp <= '9');
        }

        switch (*tmp) {
          case 'u':
          case 'd':
            if (width > 4)
              return 0;
            uintVal = va_arg(ap, int);
            Split(uintVal, g_order+offset, width);
            break;
          case 'c': // 等价于"%d"
            if (width > 1)
              return 0;
            ucharVal = va_arg(ap, int);
            g_order[offset] = ucharVal;
            break;
          case 's':
            strVal = va_arg(ap, char*);
            memcpy(g_order+offset, strVal, width);
            break;
          default:
            return 0;
        } // end switch 

        offset += width;
      } // end if (*p == '%')
    } // end for 

    Split(offset+2-9, g_order+7, 2);  // 包长度
    Split(Calibrate(g_order, offset+2), g_order+offset, 2); // 检校和
    
    va_end(ap);
    return offset + 2;
  } // end else (count != 0)
}

Car::Car()
{
	
}

Car::~Car()
{

}

void Car::pri()
{
	printf("SD");
}

bool Car::PS_Setup(uint chipAddr, uint password) {
  g_as608.chip_addr = chipAddr;
  g_as608.password  = password;

  if (g_verbose == 1)
    printf("-------------------------Initializing-------------------------\n");
  //验证密码
  if (g_as608.has_password) {
    if (!PS_VfyPwd(password))
     return false;
  }

  // 获取数据包大小、波特率等
  if (PS_ReadSysPara() && g_as608.packet_size > 0) {
    if (g_verbose == 1)
      printf("-----------------------------Done-----------------------------\n");
    return true;
  }

  if (g_verbose == 1)
    printf("-----------------------------Done-----------------------------\n");
  g_error_code = 0xC7;
  return false;
}


bool Car::PS_GetImage() {
  int size = GenOrder(0x01, "");

  // 检测是否有指纹
  //for (int i = 0; i < 100000; ++i) {
  //  if (digitalRead(g_as608.detect_pin) == HIGH) {
  //    delay(1000);
  //    printf("Sending order.. \n");
  
  // 发送指令包
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 12) && Check(g_reply, 12));

  //}
  // 100*100000=10e6微秒，即10秒
  // 如果10秒内，没有检测到指纹，返回false
  //  usleep(100);
  // }

  //g_error_code = 0x02;  // 传感器上没有手指
  //return false;
}


bool Car::PS_GenChar(uchar bufferID) {
  int size = GenOrder(0x02, "%d", bufferID);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 12) && Check(g_reply, 12));
}

bool Car::PS_Match(int* pScore) {
  int size = GenOrder(0x03, "");
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 14) && 
          Check(g_reply, 14) &&
          Merge(pScore, g_reply+10, 2));
}


bool Car::PS_Search(uchar bufferID, int startPageID, int count, int* pPageID, int* pScore) {
  int size = GenOrder(0x04, "%d%2d%2d", bufferID, startPageID, count);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return ( RecvReply(g_reply, 16) && 
           Check(g_reply, 16) && 
           (Merge(pPageID, g_reply+10, 2)) &&  // 给pageID赋值，返回true
           (Merge(pScore,  g_reply+12, 2))     // 给score赋值，返回true
        );
}

bool Car::PS_RegModel() {
  int size = GenOrder(0x05, "");
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 12) &&
      Check(g_reply, 12));
}


bool Car::PS_StoreChar(uchar bufferID, int pageID) {
  int size = GenOrder(0x06, "%d%2d", bufferID, pageID);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 12) && 
        Check(g_reply, 12));
}


bool Car::PS_LoadChar(uchar bufferID, int pageID) {
  int size = GenOrder(0x07, "%d%2d", bufferID, pageID);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  return (RecvReply(g_reply, 12) &&
         Check(g_reply, 12));
}


bool Car::PS_UpChar(uchar bufferID, const char* filename) {
  int size = GenOrder(0x08, "%d", bufferID);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  if (!(RecvReply(g_reply, 12) && Check(g_reply, 12))) {
    return false;
  }

  // 接收数据包，将有效数据存储到 pData 中
  uchar pData[768] = { 0 };
  if (!RecvPacket(pData, 768)) {
    return false;
  }

  // 写入文件
  FILE* fp = fopen(filename, "w+");
  if (!fp) { 
    g_error_code = 0xC2;
    return false;
  }

  fwrite(pData, 1, 768, fp);
  fclose(fp);

  return true;
}


bool Car::PS_DownChar(uchar bufferID, const char* filename) {
  // 发送指令
  int size = GenOrder(0x09, "%d", bufferID);
  SendOrder(g_order, size);

  // 接收应答包，如果确认码为0x00，说明可以发送后续数据包
  if ( !(RecvReply(g_reply, 12) && Check(g_reply, 12)) )
    return false;

  // 打开本地文件
  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    g_error_code = 0xC2;
    return false;
  }

  // 获取文件大小
  int fileSize = 0;
  fseek(fp, 0, SEEK_END);
  fileSize = ftell(fp);
  rewind(fp);
  if (fileSize != 768) {
    g_error_code = 0x09;
    fclose(fp);
    return false;
  }

  // 读取文件内容
  uchar charBuf[768] = { 0 };
  fread(charBuf, 1, 768, fp);

  fclose(fp);

  // 发送数据包
  return SendPacket(charBuf, 768);
}


bool Car::PS_UpImage(const char* filename) {
  int size = GenOrder(0x0a, "");
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  if (!(RecvReply(g_reply, 12) && Check(g_reply, 12))) {
    return false;
  }

  // 接收数据包，将有效数据存储到 pData 中
  // 图像尺寸 128*288 = 36864
  uchar* pData = (uchar*)malloc(36864);
  if (!RecvPacket(pData, 36864)) {
    return false;
  }

  // 将pData写入文件中
  FILE* fp = fopen(filename, "w+");
  if (!fp) {
    g_error_code = 0xC2;
    return false;
  }

  // 构造bmp头(对该模块而言，是固定的)
  uchar header[54] = "\x42\x4d\x00\x00\x00\x00\x00\x00\x00\x00\x36\x04\x00\x00\x28\x00\x00\x00\x00\x01\x00\x00\x20\x01\x00\x00\x01\x00\x08";
  for (int i = 29; i < 54; ++i)
    header[i] = 0x00;
  fwrite(header, 1, 54, fp);

  // 调色板
  uchar palette[1024] = { 0 };
  for (int i = 0; i < 256; ++i) {
    palette[4*i]   = i;
    palette[4*i+1] = i;
    palette[4*i+2] = i;
    palette[4*i+3] = 0;
  }
  fwrite(palette, 1, 1024, fp);

  // bmp像素数据
  uchar* pBody = (uchar*)malloc(73728);
  for (int i = 0; i < 73728; i += 2) {
    pBody[i] = pData[i/2] & 0xf0;  
  }
  for (int i = 1; i < 73728; i += 2) {
    pBody[i] = (pData[i/2] & 0x0f) << 4;
  }

  fwrite(pBody, 1, 73728, fp);

  free(pBody);
  free(pData);
  fclose(fp);

  return true; 
}


bool Car::PS_DownImage(const char* filename) {
  int size = GenOrder(0x0b, "");
  SendOrder(g_order, size);

  if (!RecvReply(g_reply, 12) && Check(g_reply, 12))
    return false;

  // 指纹图像文件大小 748069 bytes
  uchar imageBuf[74806] = { 0 };

  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    g_error_code = 0xC2;
    return false;
  }

  // 获取图像文件大小
  int imageSize = 0;
  fseek(fp, 0, SEEK_END);
  imageSize = ftell(fp);
  rewind(fp);
  if (imageSize != 74806) { // 指纹图像大小 必须为74806kb
    g_error_code = 0xC9;
    fclose(fp);
    return false;
  }

  // 读文件
  if (fread(imageBuf, 1, 74806, fp) != 74806) {
    g_error_code = 0xCA;
    fclose(fp);
    return false;
  }
  fclose(fp);

  //FILE* fpx = fopen("temp.bmp", "wb");
  //if (!fpx) {
  //  printf("Error\n");
  //  return false;
  //}
  //fwrite(imageBuf, 1, 74806, fpx);
  //fclose(fpx);
  //return true;

  //uchar dataBuf[128*288] = { 0 };
  //for (uint i = 0, size=128*288; i < size; ++i) {
  //  dataBuf[i] = imageBuf[1078 + i*2] + (imageBuf[1078 + i*2 + 1] >> 4);
  //}

  // 发送图像的像素数据，偏移量54+1024=1078，大小128*256*2=73728
  //return SendPacket(dataBuf, 128*288);
  return SendPacket(imageBuf+1078, 73728);
}



bool Car::PS_DeleteChar(int startPageID, int count) {
  int size = GenOrder(0x0c, "%2d%2d", startPageID, count);
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) &&
         Check(g_reply, 12));
}


bool Car::PS_Empty() {
  int size = GenOrder(0x0d, "");
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) && Check(g_reply, 12));
}


bool Car::PS_WriteReg(int regID, int value) {
  if (regID != 4 && regID != 5 && regID != 6) {
    g_error_code = 0x1a;
    return false;
  }

  int size = GenOrder(0x0e, "%d%d", regID, value);
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) && Check(g_reply, 12));
}

/*
 * 函数名称：PS_ReadSysPara
 * 说明：读取模块的基本参数（波特率，包大小等）。
 * 参数：none(参数保存到g_as608.chip_addr, g_as608.packet_size, PS_BPS等系统变量中)
 * 返回值 ：true(成功)，false(出现错误)，确认码赋值给g_error_code
 *   确认码=00H 表示 OK；
 *   确认码=01H 表示收包有错；
*/
bool Car::PS_ReadSysPara() {
  int size = GenOrder(0x0f, "");
  SendOrder(g_order, size);
  
  return (RecvReply(g_reply, 28) &&
          Check(g_reply, 28) &&
          Merge(&g_as608.status,       g_reply+10, 2) &&
          Merge(&g_as608.model,        g_reply+12, 2) && 
          Merge(&g_as608.capacity,     g_reply+14, 2) &&
          Merge(&g_as608.secure_level, g_reply+16, 2) &&
          Merge(&g_as608.chip_addr,    g_reply+18, 4) &&
          Merge(&g_as608.packet_size,  g_reply+22, 2) &&
          Merge(&g_as608.baud_rate,    g_reply+24, 2) &&
          (g_as608.packet_size = 32 * (int)pow(2, g_as608.packet_size)) &&
          (g_as608.baud_rate *= 9600)
         );
}


bool Car::PS_Enroll(int* pPageID) {
  int size = GenOrder(0x10, "");
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 14) &&
          Check(g_reply, 14) &&
          Merge(pPageID, g_reply+10, 2)
         );
}


bool Car::PS_Identify(int* pPageID, int* pScore) { 
  int size = GenOrder(0x11, "");
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 16) &&
          Check(g_reply, 16) &&
          Merge(pPageID, g_reply+10, 2) &&
          Merge(pScore,  g_reply+12, 2)
         );
}


bool Car::PS_SetPwd(uint pwd) {   // 0x00 ~ 0xffffffff
  int size  = GenOrder(0x12, "%4d", pwd);
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) && 
          Check(g_reply, 12) &&
          (g_as608.has_password = 1) &&
          ((g_as608.password = pwd) || true)); // 防止pwd=0x00
}


bool Car::PS_VfyPwd(uint pwd) { 
  int size = GenOrder(0x13, "%4d", pwd); 
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) && Check(g_reply, 12));
}


bool Car::PS_GetRandomCode(uint* pRandom) {
  int size = GenOrder(0x14, "");
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 16) &&
          Check(g_reply, 16) &&
          Merge(pRandom, g_reply+10, 4)
         );
}


bool Car::PS_SetChipAddr(uint addr) {
  int size = GenOrder(0x15, "%4d", addr);
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 12) && 
          Check(g_reply, 12) && 
          ((g_as608.chip_addr = addr) || true)); // 防止addr=0x00
}


bool Car::PS_ReadINFpage(uchar* pInfo, int pInfoSize/*>=512*/) {
  if (pInfoSize < 512) {
    g_error_code = 0xC1;
    return false;
  }

  int size = GenOrder(0x16, "");
  SendOrder(g_order, size);

  // 接收应答包
  if (!(RecvReply(g_reply, 12) && Check(g_reply, 12))) 
    return false;

  // 接收数据包
  if (!RecvPacket(pInfo, 512))
    return false;
  
  memcpy(g_as608.product_sn,       pInfo+28, 8);
  memcpy(g_as608.software_version, pInfo+36, 8);
  memcpy(g_as608.manufacture,      pInfo+44, 8);
  memcpy(g_as608.sensor_name,      pInfo+52, 8);

  return true;
}


bool Car::PS_WriteNotepad(int notePageID, uchar* pContent, int contentSize) {
  if (contentSize > 32) {
    g_error_code = 0xC6;
    return false;
  }

  pContent[32] = 0; // 字符串结束符
  int size = GenOrder(0x18, "%d%32s", notePageID, pContent);
  SendOrder(g_order, size);

  return (RecvReply(g_reply, 12) && Check(g_reply, 12));
}


bool Car::PS_ReadNotepad(int notePageID, uchar* pContent, int contentSize) {
  if (contentSize < 32) {
    g_error_code = 0xC1;
    return false;
  }

  int size = GenOrder(0x19, "%d", notePageID);
  SendOrder(g_order, size);

  // 接收应答包，核对确认码和检校和
  if (!(RecvReply(g_reply, 44) && Check(g_reply, 44)))
    return false;

  memcpy(pContent, g_reply+10, 32);
  return true;
}


bool Car::PS_HighSpeedSearch(uchar bufferID, int startPageID, int count, int* pPageID, int* pScore) {
  int size = GenOrder(0x1b, "%d%2d%2d", bufferID, startPageID, count);
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return ( RecvReply(g_reply, 16) && 
           Check(g_reply, 16) && 
           (Merge(pPageID, g_reply+10, 2)) &&  // 给pageID赋值，返回true
           (Merge(pScore,  g_reply+12, 2))     // 给score赋值，返回true
        );
}


bool Car::PS_ValidTempleteNum(int* pValidN) {
  int size = GenOrder(0x1d, "");
  SendOrder(g_order, size);

  // 接收数据，核对确认码和检校和
  return (RecvReply(g_reply, 14) &&
          Check(g_reply, 14) &&
          Merge(pValidN, g_reply+10, 2)
         );
}


bool Car::PS_ReadIndexTable(int* indexList, int size) {
  // 将indexList所有元素初始化为-1
  for (int i = 0; i < size; ++i)
    indexList[i] = -1;

  int nIndex = 0;

  for (int page = 0; page < 2; ++page) {
    // 发送数据（两次，每页256个指纹模板，需要请求两页），
    int size = GenOrder(0x1f, "%d", page);
    SendOrder(g_order, size);

    // 接收数据，核对确认码和检校和
    if (!(RecvReply(g_reply, 44) && Check(g_reply, 44)))
      return false;

    for (int i = 0; i < 32; ++i) {
      for (int j = 0; j < 8; ++j) {
        if ( ( (g_reply[10+i] & (0x01 << j) ) >> j) == 1 ) {
          if (nIndex > size) {
            g_error_code = 0xC1;    // 数组太小
            return false;
          }
          indexList[nIndex++] = page*256 + 8 * i + j;
        } // end if

      } // end internel for

    } // end middle for

  }// end outer for

  return true;
}

// 封装函数

bool Car::PS_DetectFinger() {
  return digitalRead(g_as608.detect_pin) == HIGH;
}

bool Car::PS_SetBaudRate(int value) {
  return PS_WriteReg(4, value / 9600);
}

bool Car::PS_SetSecureLevel(int level) {
  return PS_WriteReg(5, level);
}

bool Car::PS_SetPacketSize(int size) {
  int value = 0;
  printf("size=%d\n", size);
  switch (size) {
  default: 
    g_error_code = 0xC5; 
    return false;
  case 32:  value = 0; break;
  case 64:  value = 1; break;
  case 128: value = 2; break;
  case 256: value = 3; break;
  }

  return PS_WriteReg(6, value);
}

bool Car::PS_GetAllInfo()
{
	uchar buf[512] = { 0 };
	if (PS_ReadSysPara() && g_as608.packet_size > 0 && PS_ReadINFpage(buf, 512)) {
		return true;
	}
	else {
		g_error_code = 0xC7;
		return false;
	}
}

bool Car::PS_Flush()
{
	int num = 0;
	for (int i = 0; i < 3; ++i) {
		if (PS_ValidTempleteNum(&num)) {
			return true;
		}
		sleep(1);
	}
  return false;
}

// 获得错误代码g_error_code的含义，并赋值给g_error_desc
char* Car::PS_GetErrorDesc()
{
	switch (g_error_code) {
	  default:   strcpy(g_error_desc, "Undefined error"); break;
	  case 0x00: strcpy(g_error_desc, "OK"); break;
	  case 0x01: strcpy(g_error_desc, "Recive packer error"); break;
	  case 0x02: strcpy(g_error_desc, "No finger on the sensor"); break;
	  case 0x03: strcpy(g_error_desc, "Failed to input fingerprint image"); break;
	  case 0x04: strcpy(g_error_desc, "Fingerprint images are too dry and bland to be characteristic"); break;
	  case 0x05: strcpy(g_error_desc, "Fingerprint images are too wet and mushy to produce features"); break;
	  case 0x06: strcpy(g_error_desc, "Fingerprint images are too messy to be characteristic"); break;
	  case 0x07: strcpy(g_error_desc, "The fingerprint image is normal, but there are too few feature points (or too small area) to produce a feature"); break;
	  case 0x08: strcpy(g_error_desc, "Fingerprint mismatch"); break;
	  case 0x09: strcpy(g_error_desc, "Not found in fingerprint libary"); break;
	  case 0x0A: strcpy(g_error_desc, "Feature merge failed"); break;
	  case 0x0B: strcpy(g_error_desc, "The address serial number is out of the range of fingerprint database when accessing fingerprint database"); break;
	  case 0x0C: strcpy(g_error_desc, "Error or invalid reading template from fingerprint database"); break;
	  case 0x0D: strcpy(g_error_desc, "Upload feature failed"); break;
	  case 0x0E: strcpy(g_error_desc, "The module cannot accept subsequent packets"); break;
	  case 0x0F: strcpy(g_error_desc, "Failed to upload image"); break;
	  case 0x10: strcpy(g_error_desc, "Failed to delete template"); break;
	  case 0x11: strcpy(g_error_desc, "Failed to clear the fingerprint database"); break;
	  case 0x12: strcpy(g_error_desc, "Cannot enter low power consumption state"); break;
	  case 0x13: strcpy(g_error_desc, "Incorrect password"); break;
	  case 0x14: strcpy(g_error_desc, "System reset failure"); break;
	  case 0x15: strcpy(g_error_desc, "An image cannot be generated without a valid original image in the buffer"); break;
	  case 0x16: strcpy(g_error_desc, "Online upgrade failed"); break;
	  case 0x17: strcpy(g_error_desc, "There was no movement of the finger between the two collections"); break;
	  case 0x18: strcpy(g_error_desc, "FLASH reading or writing error"); break;
	  case 0x19: strcpy(g_error_desc, "Undefined error"); break;
	  case 0x1A: strcpy(g_error_desc, "Invalid register number"); break;
	  case 0x1B: strcpy(g_error_desc, "Register setting error"); break;
	  case 0x1C: strcpy(g_error_desc, "Notepad page number specified incorrectly"); break;
	  case 0x1D: strcpy(g_error_desc, "Port operation failed"); break;
	  case 0x1E: strcpy(g_error_desc, "Automatic enrollment failed"); break;
	  case 0xFF: strcpy(g_error_desc, "Fingerprint is full"); break;
	  case 0x20: strcpy(g_error_desc, "Reserved. Wrong address or wrong password"); break;
	  case 0xF0: strcpy(g_error_desc, "There are instructions for subsequent packets, and reply with 0xf0 after correct reception"); break;
	  case 0xF1: strcpy(g_error_desc, "There are instructions for subsequent packets, and the command packet replies with 0xf1"); break;
	  case 0xF2: strcpy(g_error_desc, "Checksum error while burning internal FLASH"); break;
	  case 0xF3: strcpy(g_error_desc, "Package identification error while burning internal FLASH"); break;
	  case 0xF4: strcpy(g_error_desc, "Packet length error while burning internal FLASH"); break;
	  case 0xF5: strcpy(g_error_desc, "Code length is too long to burn internal FLASH"); break;
	  case 0xF6: strcpy(g_error_desc, "Burning internal FLASH failed"); break;
	  case 0xC1: strcpy(g_error_desc, "Array is too smalll to store all the data"); break;
	  case 0xC2: strcpy(g_error_desc, "Open local file failed!"); break;
	  case 0xC3: strcpy(g_error_desc, "Packet loss"); break;
	  case 0xC4: strcpy(g_error_desc, "No end packet received, please flush the buffer(PS_Flush)"); break;
	  case 0xC5: strcpy(g_error_desc, "Packet size not in 32, 64, 128 or 256"); break;
	  case 0xC6: strcpy(g_error_desc, "Array size is to big");break;
	  case 0xC7: strcpy(g_error_desc, "Setup failed! Please retry again later"); break;
	  case 0xC8: strcpy(g_error_desc, "The size of the data to send must be an integral multiple of the g_as608.packet_size"); break;
	  case 0xC9: strcpy(g_error_desc, "The size of the fingerprint image is not 74806bytes(about73.1kb)");break;
	  case 0xCA: strcpy(g_error_desc, "Error while reading local fingerprint imgae"); break;
	  
	  }

	  return g_error_desc;
}