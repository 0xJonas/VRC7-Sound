
/*
 _     __   _____     ____   _______
| |   / /  / ___ \   / ___\ /___   /    #####    ###    ##    #  ##    #  #####
| |  / /  / /__/ /  / /       __/ /   ##       ##   #  ##    #  ###   #  ##    #
| | / /  / _   _/  / /       /_  _/   ####    ##   #  ##    #  ## #  #  ##    #
| |/ /  / / | |   / /___      / /        ##  ##   #  ##    #  ##  # #  ##    #
|___/  /_/  |_|   \____/     /_/    #####     ###     ####   ##    #  ######


VRC7 Audio emulator by Delphi1024

===RESOURCES===
VRC7 die shot by digshadow:
	https://siliconpr0n.org/archive/doku.php?id=digshadow:konami:vrc_vii_053982

YM2413 (OPLL) Datasheet:
	http://www.smspower.org/maxim/Documents/YM2413ApplicationManual

VRC7 audio on Nesdev Wiki:
	https://wiki.nesdev.com/w/index.php/VRC7_audio

OPL2 logSin/exp/ksl tables by Olli Niemitalo:
	http://yehar.com/blog/?p=665
	https://docs.google.com/document/d/18IGx18NQY_Q1PJVZ-bHywao9bhsDoAqoIn1rIm42nwo/edit

Instrument ROM Dump by NukeYKT:
	https://siliconpr0n.org/archive/doku.php?id=vendor:yamaha:opl2#ym2413_instruments

Lagrange point filter analysis by kevtris:
	http://forums.nesdev.com/viewtopic.php?t=4709&p=41523
*/

// <!>  Uncomment the following line if you want to use vrc7_sound from DLL  <!>
// #define VRC7_SOUND_IMPORTING

// <!>  Uncomment the following line when you want to have the VRC7's TEST register enabled <!>
// #define VRC7_TEST_REG

#ifndef VRC7_SOUND_H
#define VRC7_SOUND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef VRC7_SOUND_EXPORTING
#define VRC7SOUND_API __declspec(dllexport)
#elif VRC7_SOUND_IMPORTING
#define VRC7SOUND_API __declspec(dllimport)
#else
#define VRC7SOUND_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VRC7_NUM_PATCHES 16
#define VRC7_NUM_CHANNELS 6

#define VRC7_SIGNAL_CHUNK_LENGTH 72

#define VRC7_DEFAULT_CLOCK_RATE 3579545.0
#define VRC7_DEFAULT_SAMPLE_RATE 48000.0

#define MODULATOR 0
#define CARRIER 1

enum patch_sets {
	VRC7_NUKE_TONE = 0,
	VRC7_RW_TONE,
	VRC7_FT36_TONE,
	VRC7_FT35_TONE,
	VRC7_MO_TONE,
	VRC7_KT2_TONE,
	VRC7_KT1_TONE,
	OPLL_2413_TONE,
	OPLL_281B_TONE
};

struct vrc7_patch {
	uint8_t feedback;
	uint8_t total_level;
	uint8_t mult[2];
	bool vibrato[2];
	bool tremolo[2];
	bool sustained[2];
	bool rect[2];
	bool key_scale_rate[2];
	uint8_t key_scale_level[2];

	uint8_t attack_rate[2];
	uint8_t decay_rate[2];
	uint8_t sustain_level[2];
	uint8_t release_rate[2];
};

struct vrc7_slot {
	uint8_t type;
	int32_t sample;
	int32_t sample_prev;

	uint32_t phase;
	uint8_t env_stage;
	uint8_t env_value;
	bool env_enabled;
	bool restart_env;
};

struct vrc7_channel {
	uint8_t instrument;
	uint16_t fNum;
	uint8_t octave;
	uint8_t volume;
	bool sustain;
	bool trigger;

	struct vrc7_slot *slots[2];
};

struct vrc7_sound {
	struct vrc7_channel *channels[VRC7_NUM_CHANNELS];
	struct vrc7_patch *patches[VRC7_NUM_PATCHES];
	int16_t *signal;
	double clock_rate;
	double sample_rate;
	double sample_length;
	double current_time;
	uint32_t vibrato_counter;
	uint32_t tremolo_value;
	int32_t tremolo_inc;
	uint32_t envelope_counter;
	uint32_t zero_count;
	uint8_t mini_counter;
	int patch_set;
	uint8_t address;
	double fir_coeff;
	double iir_coeff;

	bool test_envelope;
	bool test_halt_phase;
	bool test_counters;

	uint32_t channel_mask;
};

VRC7SOUND_API struct vrc7_sound *vrc7_new();
VRC7SOUND_API void vrc7_delete(struct vrc7_sound *vrc7_s);
VRC7SOUND_API void vrc7_reset(struct vrc7_sound *vrc7_s);
VRC7SOUND_API void vrc7_clear(struct vrc7_sound *vrc7_s);
VRC7SOUND_API void vrc7_set_clock_rate(struct vrc7_sound *vrc7_s, double clock_rate);
VRC7SOUND_API void vrc7_set_sample_rate(struct vrc7_sound *vrc7_s, double sample_rate);
VRC7SOUND_API void vrc7_set_patch_set(struct vrc7_sound *vrc7_s, int patch_set);
VRC7SOUND_API void vrc7_tick(struct vrc7_sound *vrc7_s);
VRC7SOUND_API int16_t vrc7_fetch_sample(struct vrc7_sound *vrc7_s);

VRC7SOUND_API void vrc7_write_addr(struct vrc7_sound *vrc7_s, unsigned char addr);
VRC7SOUND_API void vrc7_write_data(struct vrc7_sound *vrc7_s, unsigned char data);

VRC7SOUND_API void vrc7_patch_to_reg(struct vrc7_patch *patch, unsigned char *reg);
VRC7SOUND_API void vrc7_reg_to_patch(unsigned const char *reg, struct vrc7_patch *patch);
VRC7SOUND_API void vrc7_get_default_patch(int set, uint32_t index, struct vrc7_patch *patch);

#ifdef _DEBUG
VRC7SOUND_API void test();
#endif

#ifdef __cplusplus
}
#endif

#endif