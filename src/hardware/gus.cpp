/*
 *  Copyright (C) 2002-2020  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosbox.h"

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string.h>

#include "inout.h"
#include "mixer.h"
#include "dma.h"
#include "pic.h"
#include "setup.h"
#include "shell.h"
#include "math.h"
#include "regs.h"
using namespace std;

// Extra bits of precision over normal gus
#define WAVE_FRACT      9
#define WAVE_FRACT_MASK ((1 << WAVE_FRACT) - 1)
#define WAVE_MSWMASK    ((1 << 16) - 1)
#define WAVE_LSWMASK    (0xffffffff ^ WAVE_MSWMASK)

#define GUS_MIN_CHANNELS     14u
#define GUS_MAX_CHANNELS     32u
#define GUS_BUFFER_FRAMES    64u
#define GUS_PAN_POSITIONS    16u // 0 face-left, 7 face-forward, and 15 face-right
#define GUS_VOLUME_POSITIONS 4096u
#define GUS_VOLUME_SCALE_DIV 1.002709201 // 0.0235 dB increments
#define GUS_RAM_SIZE         1048576u // 1 MB
#define GUS_BASE             myGUS.portbase
#define LOG_GUS              0

#define WCTRL_STOPPED       0x01
#define WCTRL_STOP          0x02
#define WCTRL_16BIT         0x04
#define WCTRL_LOOP          0x08
#define WCTRL_BIDIRECTIONAL 0x10
#define WCTRL_IRQENABLED    0x20
#define WCTRL_DECREASING    0x40
#define WCTRL_IRQPENDING    0x80

uint8_t adlib_commandreg = 0u;
static MixerChannel *gus_chan = nullptr;
static uint8_t irqtable[8] = {0, 2, 5, 3, 7, 11, 12, 15};
static uint8_t dmatable[8] = {0, 1, 3, 5, 6, 7, 0, 0};
static uint8_t GUSRam[GUS_RAM_SIZE] = {0u};
static std::array<float, GUS_VOLUME_POSITIONS> vol_scalars = {0.0f};

struct Frame {
	float left = 0.0f;
	float right = 0.0f;
};

static std::array<Frame, GUS_PAN_POSITIONS> pan_scalars = {};

class GUSChannels;
static void CheckVoiceIrq();

struct GFGus {
	uint8_t gRegSelect = 0u;
	uint16_t gRegData = 0u;
	uint32_t gDramAddr = 0u;
	uint16_t gCurChannel = 0u;

	uint8_t DMAControl = 0u;
	uint16_t dmaAddr = 0u;
	uint8_t TimerControl = 0u;
	uint8_t SampControl = 0u;
	uint8_t mixControl = 0u;
	uint8_t ActiveChannels = 0u;
	uint32_t basefreq = 0u;

	struct GusTimer {
		uint8_t value = 0u;
		bool reached = false;
		bool raiseirq = false;
		bool masked = false;
		bool running = false;
		float delay = 0.0f;
	} timers[2];

	uint32_t rate = 0u;
	Frame peak_amplitude = {1.0f, 1.0f};
	Bitu portbase = 0u;
	uint8_t dma1 = 0u;
	uint8_t dma2 = 0u;

	uint8_t irq1 = 0u;
	uint8_t irq2 = 0u;

	bool irqenabled = false;
	bool ChangeIRQDMA = false;
	// IRQ status register values
	uint8_t IRQStatus = 0u;
	uint32_t ActiveMask = 0u;
	uint8_t IRQChan = 0u;
	uint32_t RampIRQ = 0u;
	uint32_t WaveIRQ = 0u;
} myGUS;

Bitu DEBUG_EnableDebugger();

static void GUS_DMA_Callback(DmaChannel *chan, DMAEvent event);

class GUSChannels {
public:
	void WriteWaveCtrl(uint8_t val);

	typedef float (GUSChannels::*get_sample_f)() const;
	get_sample_f getSample = &GUSChannels::GetSample8;

	uint32_t WaveStart = 0u;
	uint32_t WaveEnd = 0u;
	uint32_t WaveAddr = 0u;
	uint32_t WaveAdd = 0u;
	uint8_t WaveCtrl = 3u;
	uint16_t WaveFreq = 0u;

	uint32_t StartVolIndex = 0u;
	uint32_t EndVolIndex = 0u;
	uint32_t CurrentVolIndex = 0u;
	uint32_t IncrVolIndex = 0u;

	uint8_t RampRate = 0u;
	uint8_t RampCtrl = 3u;

	uint8_t PanPot = 7u;
	uint8_t channum = 0u;
	uint32_t irqmask = 0u;

	uint32_t generated_8bit_ms = 0u;
	uint32_t generated_16bit_ms = 0u;
	uint32_t *generated_ms = &generated_8bit_ms;

	GUSChannels(uint8_t num) : channum(num), irqmask(1 << num) {}

	void ClearStats()
	{
		generated_8bit_ms = 0u;
		generated_16bit_ms = 0u;
	}

	// Fetch the next 8-bit sample from GUS memory returned as a floating
	// point type containing a value that spans the 16-bit signed range.
	// This implementation preserves up to 3 significant figures of the
	// inter-wave portion previously lost due to integer bit-shifting.
	inline float GetSample8() const
	{
		const uint32_t useAddr = WaveAddr >> WAVE_FRACT;
		float w1 = static_cast<int8_t>(GUSRam[useAddr]);
		// add a fraction of the next sample
		if (WaveAdd < (1 << WAVE_FRACT)) {
			const uint32_t nextAddr = (useAddr + 1) & (GUS_RAM_SIZE - 1);
			const float w2 = static_cast<float>(
			        static_cast<int8_t>(GUSRam[nextAddr]));
			const float diff = w2 - w1;
			constexpr float max_wave = static_cast<float>(1 << WAVE_FRACT);
			const float scale = (WaveAddr & WAVE_FRACT_MASK) / max_wave;
			w1 += diff * scale;

			// Ensure the sample with added inter-wave portion is
			// still within the true 8-bit range, albeit with far
			// more accuracy.
			assert(w1 <= std::numeric_limits<int8_t>::max() ||
			       w1 >= std::numeric_limits<int8_t>::min());
		}
		constexpr auto to_16bit_range =
		        1 << (std::numeric_limits<int16_t>::digits -
		              std::numeric_limits<int8_t>::digits);
		return w1 * to_16bit_range;
	}

	// Fetch the next 16-bit sample from GUS memory as a floating point
	inline float GetSample16() const
	{
		// Formula used to convert addresses for use with 16-bit samples
		const uint32_t base = WaveAddr >> WAVE_FRACT;
		const uint32_t holdAddr = base & 0xc0000L;
		const uint32_t useAddr = holdAddr | ((base & 0x1ffffL) << 1);

		float w1 = static_cast<float>(
		        GUSRam[useAddr] |
		        (static_cast<int8_t>(GUSRam[useAddr + 1]) << 8));

		// add a fraction of the next sample
		if (WaveAdd < (1 << WAVE_FRACT)) {
			const float w2 = static_cast<float>(
			        static_cast<int8_t>(GUSRam[useAddr + 2]) |
			        (static_cast<int8_t>(GUSRam[useAddr + 3]) << 8));
			const float diff = w2 - w1;
			constexpr float max_wave = static_cast<float>(1 << WAVE_FRACT);
			const float scale = (WaveAddr & WAVE_FRACT_MASK) / max_wave;
			w1 += diff * scale;

			// Ensure the sample with added inter-wave portion is
			// still within the true 16-bit range.
			assert(w1 <= std::numeric_limits<int16_t>::max() ||
			       w1 >= std::numeric_limits<int16_t>::min());
		}
		return w1;
	}

	void WriteWaveFreq(uint16_t val)
	{
		WaveFreq = val;
		WaveAdd = ceil_udivide(val, 2u);
	}

	inline uint8_t ReadWaveCtrl() const
	{
		uint8_t ret = WaveCtrl;
		if (myGUS.WaveIRQ & irqmask)
			ret |= 0x80;
		return ret;
	}
	void UpdateWaveRamp()
	{
		WriteWaveFreq(WaveFreq);
		WriteRampRate(RampRate);
	}

	void WritePanPot(uint8_t pos)
	{
		constexpr uint8_t max_pos = GUS_PAN_POSITIONS - 1;
		PanPot = std::min(pos, max_pos);
	}

	uint8_t ReadPanPot() const { return PanPot; }
	void WriteRampCtrl(uint8_t val)
	{
		const uint32_t old = myGUS.RampIRQ;
		RampCtrl = val & 0x7f;
		// Manually set the irq
		if ((val & 0xa0) == 0xa0)
			myGUS.RampIRQ |= irqmask;
		else
			myGUS.RampIRQ &= ~irqmask;
		if (old != myGUS.RampIRQ)
			CheckVoiceIrq();
	}
	inline uint8_t ReadRampCtrl() const
	{
		uint8_t ret = RampCtrl;
		if (myGUS.RampIRQ & irqmask)
			ret |= 0x80;
		return ret;
	}
	void WriteRampRate(uint8_t val)
	{
		RampRate = val;
		const uint8_t scale = val & 63;
		const uint8_t divider = 1 << (3 * (val >> 6));
		IncrVolIndex = (!scale || !divider) ? 0u :ceil_udivide(scale, divider);
	}
	inline void WaveUpdate()
	{
		if (WaveCtrl & (WCTRL_STOP | WCTRL_STOPPED))
			return;
		int32_t WaveLeft;
		if (WaveCtrl & WCTRL_DECREASING) {
			WaveAddr -= WaveAdd;
			WaveLeft = WaveStart - WaveAddr;
		} else {
			WaveAddr += WaveAdd;
			WaveLeft = WaveAddr - WaveEnd;
		}
		// Not yet reaching a boundary
		if (WaveLeft < 0)
			return;
		/* Generate an IRQ if needed */
		if (WaveCtrl & 0x20) {
			myGUS.WaveIRQ |= irqmask;
		}
		/* Check for not being in PCM operation */
		if (RampCtrl & 0x04)
			return;
		/* Check for looping */
		if (WaveCtrl & WCTRL_LOOP) {
			/* Bi-directional looping */
			if (WaveCtrl & WCTRL_BIDIRECTIONAL)
				WaveCtrl ^= WCTRL_DECREASING;
			WaveAddr = (WaveCtrl & WCTRL_DECREASING)
			                   ? (WaveEnd - WaveLeft)
			                   : (WaveStart + WaveLeft);
		} else {
			WaveCtrl |= 1; // Stop the channel
			WaveAddr = (WaveCtrl & WCTRL_DECREASING) ? WaveStart
			                                         : WaveEnd;
		}
	}

	inline void RampUpdate()
	{
		/* Check if ramping enabled */
		if (RampCtrl & 0x3)
			return;
		int32_t RemainingVolIndexes;
		if (RampCtrl & 0x40) {
			CurrentVolIndex -= IncrVolIndex;
			RemainingVolIndexes = StartVolIndex - CurrentVolIndex;
		} else {
			CurrentVolIndex += IncrVolIndex;
			RemainingVolIndexes = CurrentVolIndex - EndVolIndex;
		}
		if (RemainingVolIndexes < 0) {
			return;
		}
		/* Generate an IRQ if needed */
		if (RampCtrl & 0x20) {
			myGUS.RampIRQ |= irqmask;
		}
		/* Check for looping */
		if (RampCtrl & 0x08) {
			/* Bi-directional looping */
			if (RampCtrl & 0x10)
				RampCtrl ^= 0x40;
			CurrentVolIndex = (RampCtrl & 0x40)
			                          ? (EndVolIndex - RemainingVolIndexes)
			                          : (StartVolIndex +
			                             RemainingVolIndexes);
		} else {
			RampCtrl |= 1; // Stop the channel
			CurrentVolIndex = (RampCtrl & 0x40) ? StartVolIndex
			                                    : EndVolIndex;
		}
	}

	void generateSamples(float *stream, Frame &peak, uint16_t len)
	{
		if (RampCtrl & WaveCtrl & 3) // Channel is disabled
			return;

		while (len-- > 0) {
			const float sample = (this->*getSample)() *
			                     vol_scalars[CurrentVolIndex];
			*(stream++) += sample * pan_scalars[PanPot].left;
			*(stream++) += sample * pan_scalars[PanPot].right;
			peak.left = std::max(peak.left, fabs(stream[-2]));
			peak.right = std::max(peak.right, fabs(stream[-1]));
			WaveUpdate();
			RampUpdate();
		}
		(*generated_ms)++;
	}
};

void GUSChannels::WriteWaveCtrl(uint8_t val)
{
	const uint32_t oldirq = myGUS.WaveIRQ;
	WaveCtrl = val & 0x7f;
	if (WaveCtrl & WCTRL_16BIT) {
		getSample = &GUSChannels::GetSample16;
		generated_ms = &generated_16bit_ms;
	} else {
		getSample = &GUSChannels::GetSample8;
		generated_ms = &generated_8bit_ms;
	}

	if ((val & 0xa0) == 0xa0)
		myGUS.WaveIRQ |= irqmask;
	else
		myGUS.WaveIRQ &= ~irqmask;
	if (oldirq != myGUS.WaveIRQ)
		CheckVoiceIrq();
}

static std::array<GUSChannels *, GUS_MAX_CHANNELS> guschan = {nullptr};
static GUSChannels *curchan = nullptr;

static void PrintStats()
{
	// Aggregate stats from all channels
	uint32_t combined_8bit_ms = 0u;
	uint32_t combined_16bit_ms = 0u;
	uint32_t used_8bit_voices = 0u;
	uint32_t used_16bit_voices = 0u;
	for (const auto voice : guschan) {
		if (voice->generated_8bit_ms) {
			combined_8bit_ms += voice->generated_8bit_ms;
			used_8bit_voices++;
		}
		if (voice->generated_16bit_ms) {
			combined_16bit_ms += voice->generated_16bit_ms;
			used_16bit_voices++;
		}
	}
	const uint32_t combined_ms = combined_8bit_ms + combined_16bit_ms;

	// Is there enough information to be meaningful?
	if (combined_ms < 10000u ||
	    (myGUS.peak_amplitude.left + myGUS.peak_amplitude.right) < 10 ||
	    !(used_8bit_voices + used_16bit_voices))
		return;

	// Print info about the type of audio and voices used
	if (used_16bit_voices == 0u)
		LOG_MSG("GUS: Audio comprised of 8-bit samples from %u voices",
		        used_8bit_voices);
	else if (used_8bit_voices == 0u)
		LOG_MSG("GUS: Audio comprised of 16-bit samples from %u voices",
		        used_16bit_voices);
	else {
		const uint8_t ratio_8bit = ceil_udivide(100u * combined_8bit_ms,
		                                        combined_ms);
		const uint8_t ratio_16bit = ceil_udivide(100u * combined_16bit_ms,
		                                         combined_ms);
		LOG_MSG("GUS: Audio was made up of %u%% 8-bit %u-voice and "
		        "%u%% 16-bit %u-voice samples",
		        ratio_8bit, used_8bit_voices, ratio_16bit,
		        used_16bit_voices);
	}

	// Calculate and print info about the volume
	const float mixer_scalar = std::max(gus_chan->volmain[0],
	                                    gus_chan->volmain[1]);
	double peak_ratio = mixer_scalar *
	                    std::max(myGUS.peak_amplitude.left,
	                             myGUS.peak_amplitude.right) /
	                    std::numeric_limits<int16_t>::max();

	// It's expected and normal for multi-channel audio to periodically
	// accumulate beyond the max, which which is gracefully scaled without
	// distortion, so there is no need to recommend that users scale-down
	// their GUS channel.
	peak_ratio = std::min(peak_ratio, 1.0);
	LOG_MSG("GUS: Peak amplitude reached %.0f%% of max", 100 * peak_ratio);

	// Make a suggestion if the peak volume was well below 3 dB
	if (peak_ratio < 0.6) {
		const auto multiplier = static_cast<uint16_t>(
		        100.0 * static_cast<double>(mixer_scalar) / peak_ratio);
		LOG_MSG("GUS: If it should be louder, %s %u",
		        fabs(mixer_scalar - 1.0f) > 0.01f ? "adjust mixer gus to" : "use: mixer gus",
		        multiplier);
	}
}

static void GUSReset()
{
	if ((myGUS.gRegData & 0x1) == 0x1) {
		// Characterize playback before resettings
		PrintStats();

		// Reset
		adlib_commandreg = 85;
		myGUS.IRQStatus = 0;
		myGUS.timers[0].raiseirq = false;
		myGUS.timers[1].raiseirq = false;
		myGUS.timers[0].reached = false;
		myGUS.timers[1].reached = false;
		myGUS.timers[0].running = false;
		myGUS.timers[1].running = false;

		myGUS.timers[0].value = 0xff;
		myGUS.timers[1].value = 0xff;
		myGUS.timers[0].delay = 0.080f;
		myGUS.timers[1].delay = 0.320f;

		myGUS.ChangeIRQDMA = false;
		myGUS.mixControl = 0x0b; // latches enabled, LINEs disabled
		// Stop all channels
		for (const auto channel : guschan) {
			channel->CurrentVolIndex = 0u;
			channel->WriteWaveCtrl(0x1);
			channel->WriteRampCtrl(0x1);
			channel->WritePanPot(0x7);
			channel->ClearStats();
		}
		myGUS.IRQChan = 0;
		myGUS.peak_amplitude = {1.0f, 1.0f};
	}
	if ((myGUS.gRegData & 0x4) != 0) {
		myGUS.irqenabled = true;
	} else {
		myGUS.irqenabled = false;
	}
}

static inline void GUS_CheckIRQ()
{
	if (myGUS.IRQStatus && (myGUS.mixControl & 0x08))
		PIC_ActivateIRQ(myGUS.irq1);
}

static void CheckVoiceIrq()
{
	myGUS.IRQStatus &= 0x9f;
	const Bitu totalmask = (myGUS.RampIRQ | myGUS.WaveIRQ) & myGUS.ActiveMask;
	if (!totalmask)
		return;
	if (myGUS.RampIRQ)
		myGUS.IRQStatus |= 0x40;
	if (myGUS.WaveIRQ)
		myGUS.IRQStatus |= 0x20;
	GUS_CheckIRQ();
	for (;;) {
		uint32_t check = (1 << myGUS.IRQChan);
		if (totalmask & check)
			return;
		myGUS.IRQChan++;
		if (myGUS.IRQChan >= myGUS.ActiveChannels)
			myGUS.IRQChan = 0;
	}
}

static uint16_t ExecuteReadRegister()
{
	uint8_t tmpreg;
	//	LOG_MSG("Read global reg %x",myGUS.gRegSelect);
	switch (myGUS.gRegSelect) {
	case 0x41: // Dma control register - read acknowledges DMA IRQ
		tmpreg = myGUS.DMAControl & 0xbf;
		tmpreg |= (myGUS.IRQStatus & 0x80) >> 1;
		myGUS.IRQStatus &= 0x7f;
		return static_cast<uint16_t>(tmpreg << 8);
	case 0x42: // Dma address register
		return myGUS.dmaAddr;
	case 0x45: // Timer control register matches Adlib's behavior
		return static_cast<uint16_t>(myGUS.TimerControl << 8);
		break;
	case 0x49: // Dma sample register
		tmpreg = myGUS.DMAControl & 0xbf;
		tmpreg |= (myGUS.IRQStatus & 0x80) >> 1;
		return static_cast<uint16_t>(tmpreg << 8);
	case 0x80: // Channel voice control read register
		if (curchan)
			return curchan->ReadWaveCtrl() << 8;
		else
			return 0x0300;

	case 0x82: // Channel MSB start address register
		if (curchan)
			return static_cast<uint16_t>(curchan->WaveStart >> 16);
		else
			return 0x0000;
	case 0x83: // Channel LSW start address register
		if (curchan)
			return static_cast<uint16_t>(curchan->WaveStart);
		else
			return 0x0000;

	case 0x89: // Channel volume register
		if (curchan)
			return static_cast<uint16_t>(curchan->CurrentVolIndex << 4);
		else
			return 0x0000;
	case 0x8a: // Channel MSB current address register
		if (curchan)
			return static_cast<uint16_t>(curchan->WaveAddr >> 16);
		else
			return 0x0000;
	case 0x8b: // Channel LSW current address register
		if (curchan)
			return static_cast<uint16_t>(curchan->WaveAddr);
		else
			return 0x0000;

	case 0x8d: // Channel volume control register
		if (curchan)
			return curchan->ReadRampCtrl() << 8;
		else
			return 0x0300;
	case 0x8f: // General channel IRQ status register
		tmpreg = myGUS.IRQChan | 0x20;
		uint32_t mask;
		mask = 1 << myGUS.IRQChan;
		if (!(myGUS.RampIRQ & mask))
			tmpreg |= 0x40;
		if (!(myGUS.WaveIRQ & mask))
			tmpreg |= 0x80;
		myGUS.RampIRQ &= ~mask;
		myGUS.WaveIRQ &= ~mask;
		CheckVoiceIrq();
		return static_cast<uint16_t>(tmpreg << 8);
	default:
#if LOG_GUS
		LOG_MSG("Read Register num 0x%x", myGUS.gRegSelect);
#endif
		return myGUS.gRegData;
	}
}

static void GUS_TimerEvent(Bitu val)
{
	if (!myGUS.timers[val].masked)
		myGUS.timers[val].reached = true;
	if (myGUS.timers[val].raiseirq) {
		myGUS.IRQStatus |= 0x4 << val;
		GUS_CheckIRQ();
	}
	if (myGUS.timers[val].running)
		PIC_AddEvent(GUS_TimerEvent, myGUS.timers[val].delay, val);
}

static void ExecuteGlobRegister()
{
	//	if (myGUS.gRegSelect|1!=0x44) LOG_MSG("write global register %x
	// with %x", myGUS.gRegSelect, myGUS.gRegData);
	switch (myGUS.gRegSelect) {
	case 0x0: // Channel voice control register
		if (curchan)
			curchan->WriteWaveCtrl(myGUS.gRegData >> 8);
		break;
	case 0x1: // Channel frequency control register
		if (curchan)
			curchan->WriteWaveFreq(myGUS.gRegData);
		break;
	case 0x2: // Channel MSW start address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(
			        (myGUS.gRegData & 0x1fff) << 16);
			curchan->WaveStart = (curchan->WaveStart & WAVE_MSWMASK) |
			                     tmpaddr;
		}
		break;
	case 0x3: // Channel LSW start address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(myGUS.gRegData);
			curchan->WaveStart = (curchan->WaveStart & WAVE_LSWMASK) |
			                     tmpaddr;
		}
		break;
	case 0x4: // Channel MSW end address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(myGUS.gRegData & 0x1fff)
			                   << 16;
			curchan->WaveEnd = (curchan->WaveEnd & WAVE_MSWMASK) | tmpaddr;
		}
		break;
	case 0x5: // Channel MSW end address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(myGUS.gRegData);
			curchan->WaveEnd = (curchan->WaveEnd & WAVE_LSWMASK) | tmpaddr;
		}
		break;
	case 0x6: // Channel volume ramp rate register
		if (curchan) {
			uint8_t tmpdata = myGUS.gRegData >> 8;
			curchan->WriteRampRate(tmpdata);
		}
		break;
	case 0x7: // Channel volume ramp start register  EEEEMMMM
		if (curchan) {
			uint8_t tmpdata = myGUS.gRegData >> 8;
			curchan->StartVolIndex = tmpdata << 4;
		}
		break;
	case 0x8: // Channel volume ramp end register  EEEEMMMM
		if (curchan) {
			uint8_t tmpdata = myGUS.gRegData >> 8;
			curchan->EndVolIndex = tmpdata << 4;
		}
		break;
	case 0x9: // Channel current volume register
		if (curchan) {
			uint16_t tmpdata = myGUS.gRegData >> 4;
			curchan->CurrentVolIndex = tmpdata;
		}
		break;
	case 0xA: // Channel MSW current address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(myGUS.gRegData & 0x1fff)
			                   << 16;
			curchan->WaveAddr = (curchan->WaveAddr & WAVE_MSWMASK) |
			                    tmpaddr;
		}
		break;
	case 0xB: // Channel LSW current address register
		if (curchan) {
			uint32_t tmpaddr = static_cast<uint32_t>(myGUS.gRegData);
			curchan->WaveAddr = (curchan->WaveAddr & WAVE_LSWMASK) |
			                    tmpaddr;
		}
		break;
	case 0xC: // Channel pan pot register
		if (curchan)
			curchan->WritePanPot(myGUS.gRegData >> 8);
		break;
	case 0xD: // Channel volume control register
		if (curchan)
			curchan->WriteRampCtrl(myGUS.gRegData >> 8);
		break;
	case 0xE: // Set active channel register
		myGUS.gRegSelect = myGUS.gRegData >> 8; // JAZZ Jackrabbit seems
		                                        // to assume this?
		{
			unsigned requested = 1 + ((myGUS.gRegData >> 8) & 63);
			requested = clamp(requested, GUS_MIN_CHANNELS,
			                  GUS_MAX_CHANNELS);
			if (requested != myGUS.ActiveChannels) {
				myGUS.ActiveChannels = requested;
				myGUS.ActiveMask = 0xffffffffU >>
				                   (32 - myGUS.ActiveChannels);
				myGUS.basefreq = static_cast<uint32_t>(
				        0.5 + 1000000.0 / (1.619695497 *
				                           myGUS.ActiveChannels));
				gus_chan->SetFreq(myGUS.basefreq);
				LOG_MSG("GUS: Activated %u voices running at %u Hz",
				        myGUS.ActiveChannels, myGUS.basefreq);
			}
			// Always re-apply the ramp as it can change elsewhere
			for (uint8_t i = 0; i < myGUS.ActiveChannels; i++)
				guschan[i]->UpdateWaveRamp();
			gus_chan->Enable(true);
		}
		break;
	case 0x10: // Undocumented register used in Fast Tracker 2
		break;
	case 0x41: // Dma control register
		myGUS.DMAControl = static_cast<uint8_t>(myGUS.gRegData >> 8);
		GetDMAChannel(myGUS.dma1)
		        ->Register_Callback(
		                (myGUS.DMAControl & 0x1) ? GUS_DMA_Callback : 0);
		break;
	case 0x42: // Gravis DRAM DMA address register
		myGUS.dmaAddr = myGUS.gRegData;
		break;
	case 0x43: // MSB Peek/poke DRAM position
		myGUS.gDramAddr = (0xff0000 & myGUS.gDramAddr) |
		                  (static_cast<uint32_t>(myGUS.gRegData));
		break;
	case 0x44: // LSW Peek/poke DRAM position
		myGUS.gDramAddr = (0xffff & myGUS.gDramAddr) |
		                  (static_cast<uint32_t>(myGUS.gRegData >> 8)) << 16;
		break;
	case 0x45: // Timer control register.  Identical in operation to Adlib's
	           // timer
		myGUS.TimerControl = static_cast<uint8_t>(myGUS.gRegData >> 8);
		myGUS.timers[0].raiseirq = (myGUS.TimerControl & 0x04) > 0;
		if (!myGUS.timers[0].raiseirq)
			myGUS.IRQStatus &= ~0x04;
		myGUS.timers[1].raiseirq = (myGUS.TimerControl & 0x08) > 0;
		if (!myGUS.timers[1].raiseirq)
			myGUS.IRQStatus &= ~0x08;
		break;
	case 0x46: // Timer 1 control
		myGUS.timers[0].value = static_cast<uint8_t>(myGUS.gRegData >> 8);
		myGUS.timers[0].delay = (0x100 - myGUS.timers[0].value) * 0.080f;
		break;
	case 0x47: // Timer 2 control
		myGUS.timers[1].value = static_cast<uint8_t>(myGUS.gRegData >> 8);
		myGUS.timers[1].delay = (0x100 - myGUS.timers[1].value) * 0.320f;
		break;
	case 0x49: // DMA sampling control register
		myGUS.SampControl = static_cast<uint8_t>(myGUS.gRegData >> 8);
		GetDMAChannel(myGUS.dma1)
		        ->Register_Callback((myGUS.SampControl & 0x1) ? GUS_DMA_Callback
		                                                      : 0);
		break;
	case 0x4c: // GUS reset register
		GUSReset();
		break;
	default:
#if LOG_GUS
		LOG_MSG("Unimplemented global register %x -- %x",
		        myGUS.gRegSelect, myGUS.gRegData);
#endif
		break;
	}
	return;
}

static Bitu read_gus(Bitu port, Bitu iolen)
{
	//	LOG_MSG("read from gus port %x",port);
	switch (port - GUS_BASE) {
	case 0x206: return myGUS.IRQStatus;
	case 0x208:
		uint8_t tmptime;
		tmptime = 0u;
		if (myGUS.timers[0].reached)
			tmptime |= (1 << 6);
		if (myGUS.timers[1].reached)
			tmptime |= (1 << 5);
		if (tmptime & 0x60)
			tmptime |= (1 << 7);
		if (myGUS.IRQStatus & 0x04)
			tmptime |= (1 << 2);
		if (myGUS.IRQStatus & 0x08)
			tmptime |= (1 << 1);
		return tmptime;
	case 0x20a: return adlib_commandreg;
	case 0x302: return static_cast<uint8_t>(myGUS.gCurChannel);
	case 0x303: return myGUS.gRegSelect;
	case 0x304:
		if (iolen == 2)
			return ExecuteReadRegister() & 0xffff;
		else
			return ExecuteReadRegister() & 0xff;
	case 0x305: return ExecuteReadRegister() >> 8;
	case 0x307:
		if (myGUS.gDramAddr < GUS_RAM_SIZE) {
			return GUSRam[myGUS.gDramAddr];
		} else {
			return 0;
		}
	default:
#if LOG_GUS
		LOG_MSG("Read GUS at port 0x%x", port);
#endif
		break;
	}

	return 0xff;
}

static void write_gus(Bitu port, Bitu val, Bitu iolen)
{
	//	LOG_MSG("Write gus port %x val %x",port,val);
	switch (port - GUS_BASE) {
	case 0x200:
		myGUS.mixControl = static_cast<uint8_t>(val);
		myGUS.ChangeIRQDMA = true;
		return;
	case 0x208: adlib_commandreg = static_cast<uint8_t>(val); break;
	case 0x209:
		// TODO adlib_commandreg should be 4 for this to work else it
		// should just latch the value
		if (val & 0x80) {
			myGUS.timers[0].reached = false;
			myGUS.timers[1].reached = false;
			return;
		}
		myGUS.timers[0].masked = (val & 0x40) > 0;
		myGUS.timers[1].masked = (val & 0x20) > 0;
		if (val & 0x1) {
			if (!myGUS.timers[0].running) {
				PIC_AddEvent(GUS_TimerEvent,
				             myGUS.timers[0].delay, 0);
				myGUS.timers[0].running = true;
			}
		} else
			myGUS.timers[0].running = false;
		if (val & 0x2) {
			if (!myGUS.timers[1].running) {
				PIC_AddEvent(GUS_TimerEvent,
				             myGUS.timers[1].delay, 1);
				myGUS.timers[1].running = true;
			}
		} else
			myGUS.timers[1].running = false;
		break;
		// TODO Check if 0x20a register is also available on the gus
		// like on the interwave
	case 0x20b:
		if (!myGUS.ChangeIRQDMA)
			break;
		myGUS.ChangeIRQDMA = false;
		if (myGUS.mixControl & 0x40) {
			// IRQ configuration, only use low bits for irq 1
			if (irqtable[val & 0x7])
				myGUS.irq1 = irqtable[val & 0x7];
#if LOG_GUS
			LOG_MSG("Assigned GUS to IRQ %d", myGUS.irq1);
#endif
		} else {
			// DMA configuration, only use low bits for dma 1
			if (dmatable[val & 0x7])
				myGUS.dma1 = dmatable[val & 0x7];
#if LOG_GUS
			LOG_MSG("Assigned GUS to DMA %d", myGUS.dma1);
#endif
		}
		break;
	case 0x302:
		myGUS.gCurChannel = val & 31;
		curchan = guschan[myGUS.gCurChannel];
		break;
	case 0x303:
		myGUS.gRegSelect = static_cast<uint8_t>(val);
		myGUS.gRegData = 0;
		break;
	case 0x304:
		if (iolen == 2) {
			myGUS.gRegData = static_cast<uint16_t>(val);
			ExecuteGlobRegister();
		} else
			myGUS.gRegData = static_cast<uint16_t>(val);
		break;
	case 0x305:
		myGUS.gRegData = static_cast<uint16_t>((0x00ff & myGUS.gRegData) |
		                                       val << 8);
		ExecuteGlobRegister();
		break;
	case 0x307:
		if (myGUS.gDramAddr < GUS_RAM_SIZE)
			GUSRam[myGUS.gDramAddr] = static_cast<uint8_t>(val);
		break;
	default:
#if LOG_GUS
		LOG_MSG("Write GUS at port 0x%x with %x", port, val);
#endif
		break;
	}
}

static void GUS_DMA_Callback(DmaChannel *chan, DMAEvent event)
{
	if (event != DMA_UNMASKED)
		return;
	Bitu dmaaddr;
	// Calculate the dma address
	// DMA transfers can't cross 256k boundaries, so you should be safe to
	// just determine the start once and go from there Bit 2 - 0 = if DMA
	// channel is an 8 bit channel(0 - 3).
	if (myGUS.DMAControl & 0x4)
		dmaaddr = (((myGUS.dmaAddr & 0x1fff) << 1) | (myGUS.dmaAddr & 0xc000))
		          << 4;
	else
		dmaaddr = myGUS.dmaAddr << 4;
	// Reading from dma?
	if ((myGUS.DMAControl & 0x2) == 0) {
		Bitu read = chan->Read(chan->currcnt + 1, &GUSRam[dmaaddr]);
		// Check for 16 or 8bit channel
		read *= (chan->DMA16 + 1);
		if ((myGUS.DMAControl & 0x80) != 0) {
			// Invert the MSB to convert twos compliment form
			const size_t dma_end = dmaaddr + read;
			if ((myGUS.DMAControl & 0x40) == 0) {
				// 8-bit data
				for (size_t i = dmaaddr; i < dma_end; ++i)
					GUSRam[i] ^= 0x80;
			} else {
				// 16-bit data
				for (size_t i = dmaaddr + 1; i < dma_end; i += 2)
					GUSRam[i] ^= 0x80;
			}
		}
		// Writing to dma
	} else {
		chan->Write(chan->currcnt + 1, &GUSRam[dmaaddr]);
	}
	/* Raise the TC irq if needed */
	if ((myGUS.DMAControl & 0x20) != 0) {
		myGUS.IRQStatus |= 0x80;
		GUS_CheckIRQ();
	}
	chan->Register_Callback(0);
}

static bool SoftLimit(float (&in)[GUS_BUFFER_FRAMES][2],
                      int16_t (&out)[GUS_BUFFER_FRAMES][2],
                      uint16_t len)
{
	constexpr float max_allowed = static_cast<float>(
	        std::numeric_limits<int16_t>::max() - 1);

	// If our peaks are under the max, then there's no need to limit
	if (myGUS.peak_amplitude.left < max_allowed &&
	    myGUS.peak_amplitude.right < max_allowed)
		return false;

	// Calculate the percent we need to scale down the volume.  In cases
	// where one side is less than the max, it's ratio is limited to 1.0.
	const Frame ratio = {std::min(1.0f, max_allowed / myGUS.peak_amplitude.left),
	                     std::min(1.0f, max_allowed / myGUS.peak_amplitude.right)};
	for (uint8_t i = 0; i < len; ++i) {
		out[i][0] = static_cast<int16_t>(in[i][0] * ratio.left);
		out[i][1] = static_cast<int16_t>(in[i][1] * ratio.right);
	}

	// Release the limit incrementally using our existing volume scale.
	constexpr float release_amount =
	        max_allowed * (static_cast<float>(GUS_VOLUME_SCALE_DIV) - 1.0f);

	if (myGUS.peak_amplitude.left > max_allowed)
		myGUS.peak_amplitude.left -= release_amount;
	if (myGUS.peak_amplitude.right > max_allowed)
		myGUS.peak_amplitude.right -= release_amount;
	// LOG_MSG("GUS: releasing myGUS.peak_amplitude = %.2f | %.2f",
	//         static_cast<double>(myGUS.peak_amplitude.left),
	//         static_cast<double>(myGUS.peak_amplitude.right));
	return true;
}

static void GUS_CallBack(uint16_t len)
{
	assert(len <= GUS_BUFFER_FRAMES);

	float accumulator[GUS_BUFFER_FRAMES][2] = {{0}};
	for (uint8_t i = 0; i < myGUS.ActiveChannels; ++i)
		guschan[i]->generateSamples(*accumulator, myGUS.peak_amplitude, len);

	int16_t scaled[GUS_BUFFER_FRAMES][2];
	if (!SoftLimit(accumulator, scaled, len))
		for (uint8_t i = 0; i < len; ++i)
			for (uint8_t j = 0; j < 2; ++j)
				scaled[i][j] = static_cast<int16_t>(
				        accumulator[i][j]);

	gus_chan->AddSamples_s16(len, scaled[0]);
	CheckVoiceIrq();
}

// Generate logarithmic to linear volume conversion tables
static void PopulateVolScalars()
{
	double out = 1.0;
	for (uint16_t i = GUS_VOLUME_POSITIONS - 1; i > 0; --i) {
		vol_scalars.at(i) = static_cast<float>(out);
		out /= GUS_VOLUME_SCALE_DIV;
	}
	vol_scalars[0] = 0.0f;
}

/*
Constant-Power Panning
-------------------------
The GUS SDK describes having 16 panning positions (0 through 15)
with 0 representing all full left rotation through to center or
mid-point at 7, to full-right rotation at 15.  The SDK also
describes that output power is held constant through this range.

	#!/usr/bin/env python3
	import math
	print(f'Left-scalar  Pot Norm.   Right-scalar | Power')
	print(f'-----------  --- -----   ------------ | -----')
	for pot in range(16):
		norm = (pot - 7.) / (7.0 if pot < 7 else 8.0)
		direction = math.pi * (norm + 1.0 ) / 4.0
		lscale = math.cos(direction)
		rscale = math.sin(direction)
		power = lscale * lscale + rscale * rscale
		print(f'{lscale:.5f} <~~~ {pot:2} ({norm:6.3f})'\
				f' ~~~> {rscale:.5f} | {power:.3f}')

	Left-scalar  Pot Norm.   Right-scalar | Power
	-----------  --- -----   ------------ | -----
	1.00000 <~~~  0 (-1.000) ~~~> 0.00000 | 1.000
	0.99371 <~~~  1 (-0.857) ~~~> 0.11196 | 1.000
	0.97493 <~~~  2 (-0.714) ~~~> 0.22252 | 1.000
	0.94388 <~~~  3 (-0.571) ~~~> 0.33028 | 1.000
	0.90097 <~~~  4 (-0.429) ~~~> 0.43388 | 1.000
	0.84672 <~~~  5 (-0.286) ~~~> 0.53203 | 1.000
	0.78183 <~~~  6 (-0.143) ~~~> 0.62349 | 1.000
	0.70711 <~~~  7 ( 0.000) ~~~> 0.70711 | 1.000
	0.63439 <~~~  8 ( 0.125) ~~~> 0.77301 | 1.000
	0.55557 <~~~  9 ( 0.250) ~~~> 0.83147 | 1.000
	0.47140 <~~~ 10 ( 0.375) ~~~> 0.88192 | 1.000
	0.38268 <~~~ 11 ( 0.500) ~~~> 0.92388 | 1.000
	0.29028 <~~~ 12 ( 0.625) ~~~> 0.95694 | 1.000
	0.19509 <~~~ 13 ( 0.750) ~~~> 0.98079 | 1.000
	0.09802 <~~~ 14 ( 0.875) ~~~> 0.99518 | 1.000
	0.00000 <~~~ 15 ( 1.000) ~~~> 1.00000 | 1.000
*/
static void PopulatePanScalars()
{
	for (uint8_t pos = 0u; pos < GUS_PAN_POSITIONS; ++pos) {
		// Normalize absolute range [0, 15] to [-1.0, 1.0]
		const double norm = (pos - 7.0f) / (pos < 7u ? 7 : 8);
		// Convert to an angle between 0 and 90-degree, in radians
		const double angle = (norm + 1) * M_PI / 4;
		pan_scalars.at(pos).left = static_cast<float>(cos(angle));
		pan_scalars.at(pos).right = static_cast<float>(sin(angle));
		// DEBUG_LOG_MSG("GUS: pan_scalar[%u] = %f | %f", pos,
		// pan_scalars.at(pos).left, pan_scalars.at(pos).right);
	}
}

class GUS : public Module_base {
private:
	IO_ReadHandleObject ReadHandler[8];
	IO_WriteHandleObject WriteHandler[9];
	AutoexecObject autoexecline[2];
	MixerObject MixerChan = {};

public:
	GUS(Section *configuration) : Module_base(configuration)
	{
		if (!IS_EGAVGA_ARCH)
			return;
		Section_prop *section = static_cast<Section_prop *>(configuration);
		if (!section->Get_bool("gus"))
			return;
		myGUS.portbase = section->Get_hex("gusbase") - 0x200;
		int dma_val = section->Get_int("gusdma");
		if ((dma_val < 0) || (dma_val > 255))
			dma_val = 3; // sensible default
		int irq_val = section->Get_int("gusirq");
		if ((irq_val < 0) || (irq_val > 255))
			irq_val = 5; // sensible default
		myGUS.dma1 = static_cast<uint8_t>(dma_val);
		myGUS.dma2 = static_cast<uint8_t>(dma_val);
		myGUS.irq1 = static_cast<uint8_t>(irq_val);
		myGUS.irq2 = static_cast<uint8_t>(irq_val);

		// We'll leave the MIDI interface to the MPU-401
		// Ditto for the Joystick
		// GF1 Synthesizer
		ReadHandler[0].Install(0x302 + GUS_BASE, read_gus, IO_MB);
		WriteHandler[0].Install(0x302 + GUS_BASE, write_gus, IO_MB);

		WriteHandler[1].Install(0x303 + GUS_BASE, write_gus, IO_MB);
		ReadHandler[1].Install(0x303 + GUS_BASE, read_gus, IO_MB);

		WriteHandler[2].Install(0x304 + GUS_BASE, write_gus, IO_MB | IO_MW);
		ReadHandler[2].Install(0x304 + GUS_BASE, read_gus, IO_MB | IO_MW);

		WriteHandler[3].Install(0x305 + GUS_BASE, write_gus, IO_MB);
		ReadHandler[3].Install(0x305 + GUS_BASE, read_gus, IO_MB);

		ReadHandler[4].Install(0x206 + GUS_BASE, read_gus, IO_MB);

		WriteHandler[4].Install(0x208 + GUS_BASE, write_gus, IO_MB);
		ReadHandler[5].Install(0x208 + GUS_BASE, read_gus, IO_MB);

		WriteHandler[5].Install(0x209 + GUS_BASE, write_gus, IO_MB);

		WriteHandler[6].Install(0x307 + GUS_BASE, write_gus, IO_MB);
		ReadHandler[6].Install(0x307 + GUS_BASE, read_gus, IO_MB);

		// Board Only

		WriteHandler[7].Install(0x200 + GUS_BASE, write_gus, IO_MB);
		ReadHandler[7].Install(0x20A + GUS_BASE, read_gus, IO_MB);
		WriteHandler[8].Install(0x20B + GUS_BASE, write_gus, IO_MB);

		//	DmaChannels[myGUS.dma1]->Register_TC_Callback(GUS_DMA_TC_Callback);

		PopulateVolScalars();
		PopulatePanScalars();

		for (uint8_t chan_ct = 0; chan_ct < guschan.size(); chan_ct++) {
			guschan[chan_ct] = new GUSChannels(chan_ct);
		}
		// Register the Mixer CallBack
		gus_chan = MixerChan.Install(GUS_CallBack, 0, "GUS");
		myGUS.gRegData = 0x1;
		GUSReset();
		myGUS.gRegData = 0x0;
		const Bitu portat = 0x200 + GUS_BASE;

		// ULTRASND=Port,DMA1,DMA2,IRQ1,IRQ2
		// [GUS port], [GUS DMA (recording)], [GUS DMA (playback)], [GUS
		// IRQ (playback)], [GUS IRQ (MIDI)]
		ostringstream temp;
		temp << "SET ULTRASND=" << hex << setw(3) << portat << ","
		     << dec << (Bitu)myGUS.dma1 << "," << (Bitu)myGUS.dma2 << ","
		     << (Bitu)myGUS.irq1 << "," << (Bitu)myGUS.irq2 << ends;
		// Create autoexec.bat lines
		autoexecline[0].Install(temp.str());
		autoexecline[1].Install(std::string("SET ULTRADIR=") +
		                        section->Get_string("ultradir"));
	}

	~GUS()
	{
		if (!IS_EGAVGA_ARCH)
			return;
		Section_prop *section = static_cast<Section_prop *>(m_configuration);
		if (!section->Get_bool("gus"))
			return;

		myGUS.gRegData = 0x1;
		GUSReset();
		myGUS.gRegData=0x0;
	
		for (auto voice : guschan)
			delete voice;
	}
};

static GUS *test;

void GUS_ShutDown(Section * /*sec*/)
{
	delete test;
}

void GUS_Init(Section *sec)
{
	test = new GUS(sec);
	sec->AddDestroyFunction(&GUS_ShutDown, true);
}
