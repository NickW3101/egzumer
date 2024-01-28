/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#if !defined(ENABLE_OVERLAY)
	#include "ARMCM0.h"
#endif
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#if defined(ENABLE_MESSENGER) || defined(ENABLE_MESSENGER_UART)
	#include "app/messenger.h"
  	#include "external/printf/printf.h"
#endif
#include "app/uart.h"
#include "board.h"
#include "bsp/dp32g030/dma.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/aes.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "version.h"

#if defined(ENABLE_OVERLAY)
	#include "sram-overlay.h"
#endif

#ifdef ENABLE_DOCK
	#include "audio.h"
	#include "driver/keyboard.h"
	#include "driver/systick.h"
#endif
#ifdef ENABLE_SCREEN_DUMP
	#include "driver/st7565.h"
#endif

#define DMA_INDEX(x, y) (((x) + (y)) % sizeof(UART_DMA_Buffer))

typedef struct {
	uint16_t ID;
	uint16_t Size;
} Header_t;

typedef struct {
	uint8_t  Padding[2];
	uint16_t ID;
} Footer_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_0514_t;

typedef struct {
	Header_t Header;
	struct {
		char     Version[16];
		bool     bHasCustomAesKey;
		bool     bIsInLockScreen;
		uint8_t  Padding[2];
		uint32_t Challenge[4];
	} Data;
} REPLY_0514_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	uint8_t  Padding;
	uint32_t Timestamp;
} CMD_051B_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
		uint8_t  Size;
		uint8_t  Padding;
		uint8_t  Data[128];
	} Data;
} REPLY_051B_t;

typedef struct {
	Header_t Header;
	uint16_t Offset;
	uint8_t  Size;
	bool     bAllowPassword;
	uint32_t Timestamp;
	uint8_t  Data[0];
} CMD_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Offset;
	} Data;
} REPLY_051D_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t RSSI;
		uint8_t  ExNoiseIndicator;
		uint8_t  GlitchIndicator;
	} Data;
} REPLY_0527_t;

typedef struct {
	Header_t Header;
	struct {
		uint16_t Voltage;
		uint16_t Current;
	} Data;
} REPLY_0529_t;

typedef struct {
	Header_t Header;
	uint32_t Response[4];
} CMD_052D_t;

typedef struct {
	Header_t Header;
	struct {
		bool bIsLocked;
		uint8_t Padding[3];
	} Data;
} REPLY_052D_t;

typedef struct {
	Header_t Header;
	uint32_t Timestamp;
} CMD_052F_t;

#ifdef ENABLE_DOCK
	typedef struct {
		Header_t Header;
		uint8_t Key;
		uint8_t Padding;
		uint32_t Timestamp;
	} CMD_0801_t; // simulate key press

	typedef struct {
		Header_t Header;
		uint32_t MidFreq;
		uint32_t Width;
		uint16_t Density;
		uint32_t Timestamp;
	} CMD_0808_t; // scan

	typedef struct {
		Header_t Header;
		struct {
			uint8_t Length;
			uint8_t Sync;
			uint8_t Signals[100];
		} Data;
	} REPLY_0808_t; // scan reply

	typedef struct {
		Header_t Header;
		uint16_t Length;
		uint16_t RegData[50];
	} CMD_085X_t; // Set and read registers

	typedef struct {
		Header_t Header;
		uint16_t Length;
		uint8_t GPIOData[50];
	} CMD_086X_t; // Set and read GPIO bits

	typedef struct {
		Header_t Header;
		struct {
			uint16_t Register;
			uint16_t Value;
		} Data;
	} REPLY_0851_t; // read register

	typedef struct {
		Header_t Header;
		struct {
			uint8_t Gpio;
			uint8_t Bit;
		} Data;
	} REPLY_0861_t; // read GPIO bit

#endif

static const uint8_t Obfuscation[16] =
{
	0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

static union
{
	uint8_t Buffer[256];
	struct
	{
		Header_t Header;
		uint8_t Data[252];
	};
} UART_Command;

static uint32_t Timestamp;
static uint16_t gUART_WriteIndex;
static bool     bIsEncrypted = true;

static void SendReply(void *pReply, uint16_t Size)
{
	Header_t Header;
	Footer_t Footer;

	if (bIsEncrypted)
	{
		uint8_t     *pBytes = (uint8_t *)pReply;
		unsigned int i;
		for (i = 0; i < Size; i++)
			pBytes[i] ^= Obfuscation[i % 16];
	}

	Header.ID = 0xCDAB;
	Header.Size = Size;
	UART_Send(&Header, sizeof(Header));
	UART_Send(pReply, Size);

	if (bIsEncrypted)
	{
		Footer.Padding[0] = Obfuscation[(Size + 0) % 16] ^ 0xFF;
		Footer.Padding[1] = Obfuscation[(Size + 1) % 16] ^ 0xFF;
	}
	else
	{
		Footer.Padding[0] = 0xFF;
		Footer.Padding[1] = 0xFF;
	}
	Footer.ID = 0xBADC;

	UART_Send(&Footer, sizeof(Footer));
}

static void SendVersion(void)
{
	REPLY_0514_t Reply;

	Reply.Header.ID = 0x0515;
	Reply.Header.Size = sizeof(Reply.Data);
	strcpy(Reply.Data.Version, Version);
	Reply.Data.bHasCustomAesKey = bHasCustomAesKey;
	Reply.Data.bIsInLockScreen = bIsInLockScreen;
	Reply.Data.Challenge[0] = gChallenge[0];
	Reply.Data.Challenge[1] = gChallenge[1];
	Reply.Data.Challenge[2] = gChallenge[2];
	Reply.Data.Challenge[3] = gChallenge[3];

	SendReply(&Reply, sizeof(Reply));
}

static bool IsBadChallenge(const uint32_t *pKey, const uint32_t *pIn, const uint32_t *pResponse)
{
	unsigned int i;
	uint32_t     IV[4];

	IV[0] = 0;
	IV[1] = 0;
	IV[2] = 0;
	IV[3] = 0;

	AES_Encrypt(pKey, IV, pIn, IV, true);

	for (i = 0; i < 4; i++)
		if (IV[i] != pResponse[i])
			return true;

	return false;
}

// session init, sends back version info and state
// timestamp is a session id really
static void CMD_0514(const uint8_t *pBuffer)
{
	const CMD_0514_t *pCmd = (const CMD_0514_t *)pBuffer;

	Timestamp = pCmd->Timestamp;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	gSerialConfigCountDown_500ms = 12; // 6 sec
	
	// turn the LCD backlight off
	BACKLIGHT_TurnOff();

	SendVersion();
}

// read eeprom
static void CMD_051B(const uint8_t *pBuffer)
{
	const CMD_051B_t *pCmd = (const CMD_051B_t *)pBuffer;
	REPLY_051B_t      Reply;
	bool              bLocked = false;

	if (pCmd->Timestamp != Timestamp)
		return;

	gSerialConfigCountDown_500ms = 12; // 6 sec

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	memset(&Reply, 0, sizeof(Reply));
	Reply.Header.ID   = 0x051C;
	Reply.Header.Size = pCmd->Size + 4;
	Reply.Data.Offset = pCmd->Offset;
	Reply.Data.Size   = pCmd->Size;

	if (bHasCustomAesKey)
		bLocked = gIsLocked;

	if (!bLocked)
		EEPROM_ReadBuffer(pCmd->Offset, Reply.Data.Data, pCmd->Size);

	SendReply(&Reply, pCmd->Size + 8);
}

// write eeprom
static void CMD_051D(const uint8_t *pBuffer)
{
	const CMD_051D_t *pCmd = (const CMD_051D_t *)pBuffer;
	REPLY_051D_t Reply;
	bool bReloadEeprom;
	bool bIsLocked;

	if (pCmd->Timestamp != Timestamp)
		return;

	gSerialConfigCountDown_500ms = 12; // 6 sec
	
	bReloadEeprom = false;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif

	Reply.Header.ID   = 0x051E;
	Reply.Header.Size = sizeof(Reply.Data);
	Reply.Data.Offset = pCmd->Offset;

	bIsLocked = bHasCustomAesKey ? gIsLocked : bHasCustomAesKey;

	if (!bIsLocked)
	{
		unsigned int i;
		for (i = 0; i < (pCmd->Size / 8); i++)
		{
			const uint16_t Offset = pCmd->Offset + (i * 8U);

			if (Offset >= 0x0F30 && Offset < 0x0F40)
				if (!gIsLocked)
					bReloadEeprom = true;

			if ((Offset < 0x0E98 || Offset >= 0x0EA0) || !bIsInLockScreen || pCmd->bAllowPassword)
				EEPROM_WriteBuffer(Offset, &pCmd->Data[i * 8U]);
		}

		if (bReloadEeprom)
			SETTINGS_InitEEPROM();
	}

	SendReply(&Reply, sizeof(Reply));
}

// read RSSI
static void CMD_0527(void)
{
	REPLY_0527_t Reply;

	Reply.Header.ID             = 0x0528;
	Reply.Header.Size           = sizeof(Reply.Data);
	Reply.Data.RSSI             = BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
	Reply.Data.ExNoiseIndicator = BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
	Reply.Data.GlitchIndicator  = BK4819_ReadRegister(BK4819_REG_63);

	SendReply(&Reply, sizeof(Reply));
}

// read ADC
static void CMD_0529(void)
{
	REPLY_0529_t Reply;

	Reply.Header.ID   = 0x52A;
	Reply.Header.Size = sizeof(Reply.Data);

	// Original doesn't actually send current!
	BOARD_ADC_GetBatteryInfo(&Reply.Data.Voltage, &Reply.Data.Current);

	SendReply(&Reply, sizeof(Reply));
}

static void CMD_052D(const uint8_t *pBuffer)
{
	const CMD_052D_t *pCmd = (const CMD_052D_t *)pBuffer;
	REPLY_052D_t      Reply;
	bool              bIsLocked;

	#ifdef ENABLE_FMRADIO
		gFmRadioCountdown_500ms = fm_radio_countdown_500ms;
	#endif
	Reply.Header.ID   = 0x052E;
	Reply.Header.Size = sizeof(Reply.Data);

	bIsLocked = bHasCustomAesKey;

	if (!bIsLocked)
		bIsLocked = IsBadChallenge(gCustomAesKey, gChallenge, pCmd->Response);

	if (!bIsLocked)
	{
		bIsLocked = IsBadChallenge(gDefaultAesKey, gChallenge, pCmd->Response);
		if (bIsLocked)
			gTryCount++;
	}

	if (gTryCount < 3)
	{
		if (!bIsLocked)
			gTryCount = 0;
	}
	else
	{
		gTryCount = 3;
		bIsLocked = true;
	}
	
	gIsLocked            = bIsLocked;
	Reply.Data.bIsLocked = bIsLocked;

	SendReply(&Reply, sizeof(Reply));
}

// session init, sends back version info and state
// timestamp is a session id really
// this command also disables dual watch, crossband, 
// DTMF side tones, freq reverse, PTT ID, DTMF decoding, frequency offset
// exits power save, sets main VFO to upper,
static void CMD_052F(const uint8_t *pBuffer)
{
	const CMD_052F_t *pCmd = (const CMD_052F_t *)pBuffer;

	gEeprom.DUAL_WATCH                               = DUAL_WATCH_OFF;
	gEeprom.CROSS_BAND_RX_TX                         = CROSS_BAND_OFF;
	gEeprom.RX_VFO                                   = 0;
	gEeprom.DTMF_SIDE_TONE                           = false;
	gEeprom.VfoInfo[0].FrequencyReverse              = false;
	gEeprom.VfoInfo[0].pRX                           = &gEeprom.VfoInfo[0].freq_config_RX;
	gEeprom.VfoInfo[0].pTX                           = &gEeprom.VfoInfo[0].freq_config_TX;
	gEeprom.VfoInfo[0].TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
	gEeprom.VfoInfo[0].DTMF_PTT_ID_TX_MODE           = PTT_ID_OFF;
#ifdef ENABLE_DTMF_CALLING
	gEeprom.VfoInfo[0].DTMF_DECODING_ENABLE          = false;
#endif

	#ifdef ENABLE_NOAA
		gIsNoaaMode = false;
	#endif

	if (gCurrentFunction == FUNCTION_POWER_SAVE)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	gSerialConfigCountDown_500ms = 12; // 6 sec

	Timestamp = pCmd->Timestamp;

	// turn the LCD backlight off
	BACKLIGHT_TurnOff();

	SendVersion();
}

#ifdef ENABLE_DOCK

	static uint16_t	R10,R11,R12,R13,R14,R30,R37,R3D,R43,R47,R48,R7E;
	static void BackupRegisters() {
	R10 = BK4819_ReadRegister(BK4819_REG_10);
	R11 = BK4819_ReadRegister(BK4819_REG_11);
	R12 = BK4819_ReadRegister(BK4819_REG_12);
	R13 = BK4819_ReadRegister(BK4819_REG_13);
	R14 = BK4819_ReadRegister(BK4819_REG_14);
	R30 = BK4819_ReadRegister(BK4819_REG_30);
	R37 = BK4819_ReadRegister(BK4819_REG_37);
	R3D = BK4819_ReadRegister(BK4819_REG_3D);
	R43 = BK4819_ReadRegister(BK4819_REG_43);
	R47 = BK4819_ReadRegister(BK4819_REG_47);
	R48 = BK4819_ReadRegister(BK4819_REG_48);
	R7E = BK4819_ReadRegister(BK4819_REG_7E);
	}

	static void RestoreRegisters() {
	BK4819_WriteRegister(BK4819_REG_10, R10);
	BK4819_WriteRegister(BK4819_REG_11, R11);
	BK4819_WriteRegister(BK4819_REG_12, R12);
	BK4819_WriteRegister(BK4819_REG_13, R13);
	BK4819_WriteRegister(BK4819_REG_14, R14);
	BK4819_WriteRegister(BK4819_REG_30, R30);
	BK4819_WriteRegister(BK4819_REG_37, R37);
	BK4819_WriteRegister(BK4819_REG_3D, R3D);
	BK4819_WriteRegister(BK4819_REG_43, R43);
	BK4819_WriteRegister(BK4819_REG_47, R47);
	BK4819_WriteRegister(BK4819_REG_48, R48);
	BK4819_WriteRegister(BK4819_REG_7E, R7E);
	}

	static void CMD_0850(const uint8_t *pBuffer) // write to multiple registers
	{
		const CMD_085X_t *pCmd = (const CMD_085X_t *)pBuffer;
		for(int i=0, j=0; i<pCmd->Length; i++, j+=2)
		{
			BK4819_WriteRegister(pCmd->RegData[j], pCmd->RegData[j+1]);
		}
	}

	// info on each register is returned as a single packet
	static void CMD_0851(const uint8_t *pBuffer) // read multiple registers
	{
		const CMD_085X_t *pCmd = (const CMD_085X_t *)pBuffer;
		REPLY_0851_t      Reply;
		for(int i=0; i<pCmd->Length; i++)
		{
			Reply.Header.ID=0x951;
			Reply.Header.Size = sizeof(Reply.Data);
			Reply.Data.Register = pCmd->RegData[i];
			Reply.Data.Value = BK4819_ReadRegister(Reply.Data.Register);
			SendReply(&Reply, sizeof(Reply));
		}
	}

	static void CMD_0860(const uint8_t *pBuffer)
	{
		const CMD_086X_t *pCmd = (const CMD_086X_t *)pBuffer;
		for(int i=0, j=0; i<pCmd->Length; i++, j+=2)
		{
			const uint8_t bit = pCmd->GPIOData[j+1];
			switch(pCmd->GPIOData[j])
			{
				case 0:
					GPIO_SetBit(&GPIOA->DATA, bit);
					break;
				case 1:
					GPIO_SetBit(&GPIOB->DATA, bit);
					break;
				case 2:
					GPIO_SetBit(&GPIOC->DATA, bit);
					break;
				case 3:
					GPIO_ClearBit(&GPIOA->DATA, bit);
					break;
				case 4:
					GPIO_ClearBit(&GPIOB->DATA, bit);
					break;
				case 5:
					GPIO_ClearBit(&GPIOC->DATA, bit);
					break;
			}
		}
	}

	static void CMD_0861(const uint8_t *pBuffer)
	{
		const CMD_086X_t *pCmd = (const CMD_086X_t *)pBuffer;
		REPLY_0861_t      Reply;
		for(int i=0, j=0; i<pCmd->Length; i++, j+=2)
		{
			Reply.Header.ID = 0x961;
			Reply.Header.Size = sizeof(Reply.Data);
			uint8_t bit;
			switch(pCmd->GPIOData[j])
			{
				case 0:
					bit=GPIO_CheckBit(&GPIOA->DATA, pCmd->GPIOData[j+1]);					
					break;
				case 1:
					bit=GPIO_CheckBit(&GPIOB->DATA, pCmd->GPIOData[j+1]);					
					break;
				case 2:
					bit=GPIO_CheckBit(&GPIOC->DATA, pCmd->GPIOData[j+1]);					
					break;
			}
			Reply.Data.Gpio = pCmd->GPIOData[j] + (bit?0:3);
			Reply.Data.Bit = pCmd->GPIOData[j+1];
			SendReply(&Reply, sizeof(Reply));
		}
	}

	static void CMD_0870() // enter hardware control mode
	{
		FUNCTION_Select(FUNCTION_FOREGROUND);
		BackupRegisters();
		while(true) // sit in a loop just executing serial commands
		{
			if (UART_IsCommandAvailable())
			{
				if(UART_Command.Header.ID == 0x871) // exit h/w control
					break;
				if(UART_Command.Header.ID != 0x870) // prevent recursion
					UART_HandleCommand();
			}
			SYSTICK_DelayUs(100); // loop delay
		}
		RestoreRegisters();
		RADIO_SetupRegisters(false);
		gSimulateKey = 13;
		gSimulateHold = 19;
		gDebounceDefeat = 0;				
		return;		
	}

	static void CMD_0801(const uint8_t *pBuffer) // smulate a key press
	{
		const CMD_0801_t *pCmd = (const CMD_0801_t *)pBuffer;
		const uint8_t key = pCmd->Key & 0x1f;
		const bool click = pCmd->Key & 32;
		if(key != KEY_INVALID)
		{
			gSimulateKey = key;
			gDebounceDefeat = 0;
			if(key == KEY_PTT)
				gPttCounter = 40;
		}
		gSimulateHold = click ? KEY_INVALID : key;
	}	

	// scan, this command is blocking, it will continue to run until another serial command is receieved
	static void CMD_0808(const uint8_t *pBuffer)
	{
		const CMD_0808_t *pCmd = (const CMD_0808_t *)pBuffer; // pointer to the command data
		if(pCmd->Density<0x300 && pCmd->Density>0) // check the density is within limits
		{
			FUNCTION_Select(FUNCTION_FOREGROUND);
			BackupRegisters();
			// dunno, just copying what other stuff does
			RADIO_SetupAGC(false, false);
			AUDIO_AudioPathOff();
			BK4819_WriteRegister(BK4819_REG_30, BK4819_ReadRegister(BK4819_REG_30) & 0xFDFF);
			BK4819_WriteRegister(BK4819_REG_47, BK4819_ReadRegister(BK4819_REG_47) & 0xFEFF);
			BK4819_SetFilterBandwidth(BK4819_FILTER_BW_WIDE, false);
			// frequency step
			uint32_t step = pCmd->Width;
			// number of steps (ensure value is odd)
			uint32_t steps = pCmd->Density | 1;
			// start frequency
			uint32_t startFreq = pCmd->MidFreq - ((steps>>1)*step);
			// may as well re-use the receive buffer
			REPLY_0808_t* Reply = (REPLY_0808_t*)pBuffer;
			// loop forever (ish)
			while(true)
			{
				if (UART_IsCommandAvailable()) // serial data is incoming
				{
					if(UART_Command.Header.ID == 0x809) // command to adjust scan 
					{
						pCmd = (const CMD_0808_t *)UART_Command.Buffer;
						if(pCmd->Density == 0) // a zero density means to end the scan and change VFO frequency
						{
							RestoreRegisters();
							gCurrentVfo->pRX->Frequency = pCmd->MidFreq;
							gCurrentVfo->pTX->Frequency = pCmd->MidFreq;
							RADIO_SetupRegisters(false);
							gSimulateKey = 13;
							gSimulateHold = 19;
							gDebounceDefeat = 0;
							return;
						}
						step = pCmd->Width;
						steps = pCmd->Density | 1;
						startFreq = pCmd->MidFreq - ((steps>>1)*step);
					}
					else // anything else means to stop scanning
					{
						// a hello response is sent to indicate exit from scanning mode
						SendVersion();
						// restore the radio's configuration
						RestoreRegisters();
						RADIO_SetupRegisters(false);
						gSimulateKey = 13;
						gSimulateHold = 19;
						gDebounceDefeat = 0;				
						return;
					}
				}
				uint8_t sync = 0; // packet number, each reply packet is 100 steps long, for larger step ranges replies are split into multiple packets
				int icnt = 0; // reply data packet index counter
				uint32_t freq = startFreq; // starting frequency
				for(uint32_t tot = 0; tot < steps; freq += step, tot++) // loop through all steps
				{				
					BK4819_SetFrequency((uint32_t)freq); // set to the current step frequency
					BK4819_PickRXFilterPathBasedOnFrequency((uint32_t)freq); // dunno, everything else does it. Seems pretty self-explanitory though.
					// seems to be needed to kick the RX in on the new frequency
					uint16_t reg = BK4819_ReadRegister(BK4819_REG_30);
					BK4819_WriteRegister(BK4819_REG_30, 0);
					BK4819_WriteRegister(BK4819_REG_30, reg);
					// set all the AGC tables to maximum gain
					BK4819_WriteRegister(BK4819_REG_10, R10 & 0x3ff);
					BK4819_WriteRegister(BK4819_REG_11, R11 & 0x3ff);
					BK4819_WriteRegister(BK4819_REG_12, R12 & 0x3ff);
					BK4819_WriteRegister(BK4819_REG_13, R13 & 0x3ff);
					BK4819_WriteRegister(BK4819_REG_14, R14 & 0x3ff);				
					SYSTICK_DelayUs(100); // short delay to allow rssi to build up
					uint16_t sig = BK4819_GetRSSI(); // get the signal level
					Reply->Data.Signals[icnt++]=(uint8_t)(sig>255?255:sig); // store the signal in the reply buffer, clamp it to 255
					if(icnt>=100 || tot>=steps-1) // check if we need to send a reply yet
					{
						// set reply params
						Reply->Header.ID = 0x908;
						Reply->Header.Size = sizeof(Reply->Data);	
						Reply->Data.Sync = sync;
						Reply->Data.Length = icnt;
						SendReply(Reply, 106); // send the reply
						icnt=0; // reset reply packet index counter
						sync++; // increment the packet number
					}
				}
			}
		}
	}

	void UART_SendUiElement(uint8_t type, uint32_t value1, uint32_t value2, uint32_t value3, uint32_t Length, const void* data)
	{
		if(gSetting_Remote_UI)
		{
			const uint8_t id = 0xB5;
			UART_Send(&id, 1);
			UART_Send(&type, 1);
			UART_Send(&value1, 1);
			UART_Send(&value2, 1);
			UART_Send(&value3, 1);
			UART_Send(&Length, 1);
			UART_Send(data, Length);
		}
	}
#endif

#ifdef ENABLE_SCREEN_DUMP
static void CMD_0803() // dumps the LCD screen memory to the PC. Not used in the Dock, is just for debug purposes
{
	const uint16_t screenDumpIdByte = 0xEFAB;
	UART_Send(&screenDumpIdByte, 2);
	UART_Send(gStatusLine, 128);
	UART_Send(gFrameBuffer, 896);
}
#endif

#ifdef ENABLE_UART_RW_BK_REGS
static void CMD_0601_ReadBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct  __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
	} CMD_0601_t;

	CMD_0601_t *cmd = (CMD_0601_t*) pBuffer;

	struct __attribute__((__packed__)) {
		Header_t header;
		struct __attribute__((__packed__)) {
			uint8_t reg;
			uint16_t value;
		} data;
	} reply;

	reply.header.ID = 0x0601;
	reply.header.Size = sizeof(reply.data);
	reply.data.reg = cmd->reg;
	reply.data.value = BK4819_ReadRegister(cmd->reg);
	SendReply(&reply, sizeof(reply));
}

static void CMD_0602_WriteBK4819Reg(const uint8_t *pBuffer)
{
	typedef struct __attribute__((__packed__)) {
		Header_t header;
		uint8_t reg;
		uint16_t value;
	} CMD_0602_t;

	CMD_0602_t *cmd = (CMD_0602_t*) pBuffer;
	BK4819_WriteRegister(cmd->reg, cmd->value);
}
#endif

#if defined(ENABLE_MESSENGER) || defined(ENABLE_MESSENGER_UART)
void remove(char cstring[], char letter) {
    for(int i = 0; cstring[i] != '\0'; i++) {
        if(cstring[i] == letter) cstring[i] = '\0';
    } 
}
#endif

bool UART_IsCommandAvailable(void)
{
	uint16_t Index;
	uint16_t TailIndex;
	uint16_t Size;
	uint16_t CRC;
	uint16_t CommandLength;
	uint16_t DmaLength = DMA_CH0->ST & 0xFFFU;

	while (1)
	{
		if (gUART_WriteIndex == DmaLength)
			return false;

#if defined(ENABLE_MESSENGER) || defined(ENABLE_MESSENGER_UART)

		if ( UART_DMA_Buffer[gUART_WriteIndex] == 'S' && UART_DMA_Buffer[gUART_WriteIndex + 1] == 'M' && UART_DMA_Buffer[ gUART_WriteIndex + 2] == 'S' && UART_DMA_Buffer[gUART_WriteIndex + 3] == ':') {
		
			char txMessage[TX_MSG_LENGTH + 4];
			memset(txMessage, 0, sizeof(txMessage));
			snprintf(txMessage, (TX_MSG_LENGTH + 4), "%s", &UART_DMA_Buffer[gUART_WriteIndex + 4]);

			remove(txMessage, '\n');
			remove(txMessage, '\r');      

			if (strlen(txMessage) > 0) {        
				MSG_Send(txMessage, false);
				UART_printf("SMS>%s\r\n", txMessage);
				gUpdateDisplay = true;
			}			
		}
  
#endif  
		while (gUART_WriteIndex != DmaLength && UART_DMA_Buffer[gUART_WriteIndex] != 0xABU)
			gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);

		if (gUART_WriteIndex == DmaLength)
			return false;

		if (gUART_WriteIndex < DmaLength)
			CommandLength = DmaLength - gUART_WriteIndex;
		else
			CommandLength = (DmaLength + sizeof(UART_DMA_Buffer)) - gUART_WriteIndex;

		if (CommandLength < 8)
			return 0;

		if (UART_DMA_Buffer[DMA_INDEX(gUART_WriteIndex, 1)] == 0xCD)
			break;

		gUART_WriteIndex = DMA_INDEX(gUART_WriteIndex, 1);
	}

	Index = DMA_INDEX(gUART_WriteIndex, 2);
	Size  = (UART_DMA_Buffer[DMA_INDEX(Index, 1)] << 8) | UART_DMA_Buffer[Index];

	if ((Size + 8u) > sizeof(UART_DMA_Buffer))
	{
		gUART_WriteIndex = DmaLength;
		return false;
	}

	if (CommandLength < (Size + 8))
		return false;

	Index     = DMA_INDEX(Index, 2);
	TailIndex = DMA_INDEX(Index, Size + 2);

	if (UART_DMA_Buffer[TailIndex] != 0xDC || UART_DMA_Buffer[DMA_INDEX(TailIndex, 1)] != 0xBA)
	{
		gUART_WriteIndex = DmaLength;
		return false;
	}

	if (TailIndex < Index)
	{
		const uint16_t ChunkSize = sizeof(UART_DMA_Buffer) - Index;
		memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, ChunkSize);
		memcpy(UART_Command.Buffer + ChunkSize, UART_DMA_Buffer, TailIndex);
	}
	else
		memcpy(UART_Command.Buffer, UART_DMA_Buffer + Index, TailIndex - Index);

	TailIndex = DMA_INDEX(TailIndex, 2);
	if (TailIndex < gUART_WriteIndex)
	{
		memset(UART_DMA_Buffer + gUART_WriteIndex, 0, sizeof(UART_DMA_Buffer) - gUART_WriteIndex);
		memset(UART_DMA_Buffer, 0, TailIndex);
	}
	else
		memset(UART_DMA_Buffer + gUART_WriteIndex, 0, TailIndex - gUART_WriteIndex);

	gUART_WriteIndex = TailIndex;

	if (UART_Command.Header.ID == 0x0514)
		bIsEncrypted = false;

	if (UART_Command.Header.ID == 0x6902)
		bIsEncrypted = true;

	if (bIsEncrypted)
	{
		unsigned int i;
		for (i = 0; i < (Size + 2u); i++)
			UART_Command.Buffer[i] ^= Obfuscation[i % 16];
	}
	
	CRC = UART_Command.Buffer[Size] | (UART_Command.Buffer[Size + 1] << 8);

	return (CRC_Calculate(UART_Command.Buffer, Size) != CRC) ? false : true;
}

void UART_HandleCommand(void)
{
	switch (UART_Command.Header.ID)
	{
		case 0x0514:
			CMD_0514(UART_Command.Buffer);
			break;
	
		case 0x051B:
			CMD_051B(UART_Command.Buffer);
			break;
	
		case 0x051D:
			CMD_051D(UART_Command.Buffer);
			break;
	
		case 0x051F:	// Not implementing non-authentic command
			break;
	
		case 0x0521:	// Not implementing non-authentic command
			break;
	
		case 0x0527:
			CMD_0527();
			break;
	
		case 0x0529:
			CMD_0529();
			break;
	
		case 0x052D:
			CMD_052D(UART_Command.Buffer);
			break;
	
		case 0x052F:
			CMD_052F(UART_Command.Buffer);
			break;
	
		case 0x05DD: // reset
			#if defined(ENABLE_OVERLAY)
				overlay_FLASH_RebootToBootloader();
			#else
				NVIC_SystemReset();
			#endif
			break;
			
#ifdef ENABLE_UART_RW_BK_REGS
		case 0x0601:
			CMD_0601_ReadBK4819Reg(UART_Command.Buffer);
			break;
		
		case 0x0602:
			CMD_0602_WriteBK4819Reg(UART_Command.Buffer);
			break;
#endif
#ifdef ENABLE_SCREEN_DUMP
		case 0x0803: // screen dump
			CMD_0803();
			break;
#endif
#ifdef ENABLE_DOCK
		case 0x0801: // simulate key press
			CMD_0801(UART_Command.Buffer);
			break;

		case 0x0808: // scan
			CMD_0808(UART_Command.Buffer);
			break;

		case 0x0850:
			CMD_0850(UART_Command.Buffer);
			break;

		case 0x0851:
			CMD_0851(UART_Command.Buffer);
			break;

		case 0x0860:
			CMD_0860(UART_Command.Buffer);
			break;

		case 0x0861:
			CMD_0861(UART_Command.Buffer);
			break;

		case 0x0870: // full control mode
			CMD_0870();
			break;
#endif

	}
}
