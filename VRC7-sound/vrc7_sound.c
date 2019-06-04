
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

#include "vrc7_sound.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define BIT_TEST(a,b) ((a & (1<<(b)))!=0)

#define PI 3.141592653589793238462643383279502884197169399

/*
==================================================
         VRC7 SOUND TABLES & CONSTANTS
==================================================
*/

#define LOGSIN_TABLE_LEN 256
#define EXP_TABLE_LEN 256
#define FAST_EXP_TABLE_LEN 4096

#define VRC7_NUM_PATCH_SETS 9

//1 + 240,000 / 4,300
#define VRC7_AMPLIFIER_GAIN 56.81395349

static unsigned const char DEFAULT_INST[VRC7_NUM_PATCH_SETS][(16 + 3) * 16] = {
  {
#include "patch-sets/vrc7tone_nuke.h"
  },
  {
#include "patch-sets/vrc7tone_rw.h"
  },
  {
#include "patch-sets/vrc7tone_ft36.h"
  },
  {
#include "patch-sets/vrc7tone_ft35.h"
  },
  {
#include "patch-sets/vrc7tone_mo.h"
  },
  {
#include "patch-sets/vrc7tone_kt2.h"
  },
  {
#include "patch-sets/vrc7tone_kt1.h"
  },
  {
#include "patch-sets/2413tone.h" 
  },
  {
#include "patch-sets/281btone.h"
  },
};

static const double MULT[16] = {0.125, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.5, 3.0, 3.0, 3.75, 3.75};

static const uint16_t KSL[16] = { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };

static const uint8_t KSL_INDEX[4] = { 3,1,2,0 };

static const int8_t FEEDBACK_SHIFT[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };

static const uint8_t TYPE_SCHEDULE[18] = { MODULATOR,MODULATOR,
										CARRIER,CARRIER ,CARRIER ,
										MODULATOR,MODULATOR ,MODULATOR ,
										CARRIER ,CARRIER ,CARRIER,
										MODULATOR,MODULATOR ,MODULATOR ,
										CARRIER ,CARRIER ,CARRIER,
										MODULATOR };

static const uint8_t CHANNEL_SCHEDULE[18] = { 1,2,0,1,2,3,4,5,3,4,5,6,7,8,6,7,8,0 };

static const bool ENV_TABLE[4][4] = {
	{false,false,false,false},
	{true ,false,false,false},
	{true ,false,true ,false},
	{true ,true ,true ,false},
};

#define ENV_PERCUSSIVE_RATE 7
#define ENV_SUSTAINED_RATE 5
#define ENV_DAMPING_RATE 12

enum env_stages {
	ENV_ATTACK=0,	//Attack phase
	ENV_DECAY,		//Above sustain level
	ENV_RELEASE,	//Below sustain level, trigger on
	ENV_DAMPING		//trigger off
};

static uint32_t logsin[LOGSIN_TABLE_LEN];

static uint16_t fast_exp[FAST_EXP_TABLE_LEN];

static void make_tables() {
	static bool tables_initialized = false;
	if (tables_initialized)
		return;

	int *exp = malloc(EXP_TABLE_LEN * sizeof(int));

	for (int i = 0; i < LOGSIN_TABLE_LEN; i++) {
		logsin[i] = (int)round(-log2(sin(((double)i + 0.5)*PI / 512.0))*256.0);
		exp[i] = (int)round((pow(2, (double)i / 256.0) - 1) * 1024);
	}

	for (uint32_t i = 0; i < FAST_EXP_TABLE_LEN; i++) {
		int shift = i >> 8;
		int index = ~i & 0xff;
		if (shift > 12) {
			fast_exp[i] = 0;
		}else {
			int linear = exp[index];
			linear += 1024;
			linear >>= shift;
			fast_exp[i] = linear & 0x7ff;
		}
	}

	free(exp);
	tables_initialized = true;
}

static inline uint32_t phase_to_logsin(uint32_t phase) {
	//phase ^= 1;	//TODO check?
	if (BIT_TEST(phase, 8))
		phase = ~phase;
	return logsin[phase & 0xff];
}

/*
==================================================
             VRC7 SOUND EMULATION
==================================================
*/

static int32_t vibrato_calc(uint32_t vibrato_counter, uint32_t fNum, uint32_t octave) {
	int32_t vib_value = 0;
	if (BIT_TEST(vibrato_counter, 11))
		vib_value = fNum >> 6;
	else if (BIT_TEST(vibrato_counter, 10))
		vib_value = fNum >> 7;
	if (BIT_TEST(vibrato_counter, 12))
		vib_value = -vib_value;
	return vib_value<<(octave+1);
}

static uint32_t update_phase(uint32_t phase, uint32_t fNum, uint32_t octave, uint32_t mult, int32_t vibrato_value) {
	int phase_inc = fNum << (octave + 2);

	phase_inc += vibrato_value;

	phase_inc = (int) (phase_inc*MULT[mult]);

	return (phase + phase_inc) & 0x7ffff;
}

static int32_t operator_calc(uint32_t phase, int32_t mod_phase, uint32_t volume,bool rect) {
	phase = ((phase >> 9) + mod_phase) & 0x3ff;
	uint32_t logsin_val = phase_to_logsin(phase);
	logsin_val += volume << 4;

	int32_t output;
	if (logsin_val >= (1 << 12)) {
		output = 0;
	}else {
		output = fast_exp[logsin_val];
	}

	//Invert or zero output when sign bit is set
	if (BIT_TEST(phase, 9)) {
		if (rect)
			output = 0;
		else
			output = ~output;
	}
	return output;
}

static uint32_t envelope_calc(struct vrc7_sound *vrc7_s, uint32_t ch,uint8_t type) {
	struct vrc7_channel *channel = vrc7_s->channels[ch];
	struct vrc7_patch *patch = vrc7_s->patches[channel->instrument];
	struct vrc7_slot *slot = channel->slots[type];
	
	//Compute low rate
	uint8_t ksr = (channel->octave << 1) + (channel->fNum >> 8);
	if (!patch->key_scale_rate[type])
		ksr >>= 2;

	//Compute high rate
	uint8_t rate_high = 0, rate_low = ksr & 0b11;
	
	switch (slot->env_stage) {
	case ENV_ATTACK:
		rate_high = patch->attack_rate[type];
		break;
	case ENV_DECAY:
		rate_high = patch->decay_rate[type];
		break;
	case ENV_RELEASE:
		if (patch->sustained[type])
			rate_high = 0;
		else
			rate_high = patch->release_rate[type];
		break;
	case ENV_DAMPING:
		if (channel->trigger)	//New note has started, get envelope ready
			rate_high = ENV_DAMPING_RATE;
		else {
			rate_high = patch->release_rate[type];
			if (channel->sustain)
				rate_high = ENV_SUSTAINED_RATE;
			else if (!patch->sustained[type])
				rate_high = ENV_PERCUSSIVE_RATE;
		}
		break;
	}
	
	rate_high += (ksr >> 2);
	rate_high = min(rate_high, 15);

	//Check whether to update the envelope 'naturally'
	bool clock_envelope = false;
	if (rate_high != 0 && rate_high < 12) {
		if (rate_high + vrc7_s->zero_count == 12)
			clock_envelope = true;
		else if (rate_high + vrc7_s->zero_count == 13 && BIT_TEST(rate_low, 1))
			clock_envelope = true;
		else if (rate_high + vrc7_s->zero_count == 14 && BIT_TEST(rate_low, 0))
			clock_envelope = true;
	}

	//Get various flags
	bool env_table = ENV_TABLE[rate_low][vrc7_s->envelope_counter & 3];
	bool mini_zero = vrc7_s->mini_counter == 0;
	bool mini_odd = (vrc7_s->mini_counter & 1) == 0;

	//Setup the four different increment values
	int inc1 = slot->env_stage == ENV_ATTACK ? slot->env_value >> 1 : 0x7f;
	int inc2 = slot->env_stage == ENV_ATTACK ? slot->env_value >> 2 : 0x7f;
	int inc3 = slot->env_stage == ENV_ATTACK ? slot->env_value >> 3 : 0x7f;
	int inc4 = slot->env_stage == ENV_ATTACK ? slot->env_value >> 4 : 0x7f;
	if (slot->env_enabled) {
		inc1 &= 0b1111101;
		inc2 &= 0b1111110;
	}
	
	//Compute envelope increment based on previous stuff
	int env_inc = 0x7f;

	if (clock_envelope || !env_table && rate_high == 12)
		env_inc &= inc4;

	if (!env_table && rate_high == 13 || env_table && rate_high == 12)
		env_inc &= inc3;

	if (clock_envelope && mini_zero && slot->env_enabled
			|| rate_high == 14 && !env_table
			|| rate_high == 13 && env_table
			|| rate_high == 13 && !env_table && mini_odd && slot->env_enabled
			|| rate_high == 12 && !env_table && mini_zero && slot->env_enabled
			|| rate_high == 12 && env_table && mini_odd && slot->env_enabled)
		env_inc &= inc2;

	if (rate_high == 15 || rate_high == 14 && env_table)
		env_inc &= inc1;

	//Update the envelope value in a weird way
	slot->env_value = ~((0x7f ^ slot->env_value) + env_inc + 1) & 0x7f;
	
	//Update envelope stage
	//Restart envelope
	if (slot->restart_env) {
		slot->env_enabled = true;
		slot->env_stage = ENV_DAMPING;
		slot->restart_env = false;
	}
	
	//Skip attack phase when rate is 15
	if (slot->env_stage == ENV_ATTACK && rate_high == 15) {
		slot->env_value = 0;
		slot->env_stage = ENV_DECAY;
	}

	//Enter decay phase when envelope peak is reached
	if (slot->env_stage == ENV_ATTACK && slot->env_value == 0) {
		slot->env_stage = ENV_DECAY;
	}

	//Exit damping phase when value is low enough
	if (slot->env_stage == ENV_DAMPING && slot->env_value >= 0x7c) {
		slot->env_stage = ENV_ATTACK;
	}

	//Check if sustain level has been reached
	uint8_t sustain_level = patch->sustain_level[type];
	if (slot->env_stage == ENV_DECAY && slot->env_value >> 3 == sustain_level) {
		slot->env_stage = ENV_RELEASE;
	}

	//Release envelope when trigger bit is 0
	if (!channel->trigger && !(type==MODULATOR && patch->sustained[MODULATOR])) {
		slot->env_stage = ENV_DAMPING;
		slot->env_enabled = true;
	}

	//Stop incrementing envelope during the RELEASE/DAMPING phase when the value gets too big
	if ((slot->env_stage == ENV_RELEASE || slot->env_stage == ENV_DAMPING) && slot->env_value >= 0x7c) {
		slot->env_enabled = false;
	}
	
	return slot->env_value;
}

static int32_t update_slot(struct vrc7_sound *vrc7_s, uint32_t ch, uint8_t type) {
	struct vrc7_channel *channel = vrc7_s->channels[ch];
	struct vrc7_patch *patch = vrc7_s->patches[channel->instrument];
	struct vrc7_slot *slot = channel->slots[type];

	int32_t vibrato_val = vibrato_calc(vrc7_s->vibrato_counter, channel->fNum, channel->octave);
	int32_t ksl_val = KSL[channel->fNum >> 5] - ((0b111 ^ channel->octave) << 3) - 8;
	ksl_val = max(ksl_val, 0);

	//Compute modulation and the first part of the volume since these values are different for modulator and carrier
	int32_t modulation = 0;
	int volume = 0;
	if (type == CARRIER) {
		modulation = channel->slots[MODULATOR]->sample<<1;
		volume = channel->volume << 3;
	}else {
		//Modulator feedback
		int8_t feedback = FEEDBACK_SHIFT[patch->feedback];
		if (patch->feedback != 0) {
			modulation = (slot->sample + slot->sample_prev) >> 1;
			modulation >>= feedback;
		}
		volume = patch->total_level << 1;
	}

	//Apply key scaling to volume level
	if (patch->key_scale_level[type] != 0) {
		ksl_val <<= 1;
		volume += ksl_val >> KSL_INDEX[patch->key_scale_level[type]];
	}

	//Add tremolo
	if (patch->tremolo[type])
		volume += vrc7_s->tremolo_value >> 3;

	//Add envelope
	uint32_t env_value = envelope_calc(vrc7_s, ch, type);
#ifdef VRC7_TEST_REG
	if (vrc7_s->test_envelope)
		env_value = 0;
#endif
	volume += env_value;

	//Clamp volume
	volume = min(volume, 0x7f);

	//Get operator value
	int32_t output = operator_calc(slot->phase, modulation, volume, patch->rect[type]);
	if (env_value == 0x7f)
		output = 0;
	slot->sample_prev = slot->sample;
	slot->sample = output;

	//Update operator phase
	slot->phase = update_phase(slot->phase,
							channel->fNum,
							channel->octave,
							patch->mult[type],
							patch->vibrato[type] ? vibrato_val : 0);

	return output;
}

static void update_fmam(struct vrc7_sound *vrc7_s) {
	//Update vibrato counter
	vrc7_s->vibrato_counter = (vrc7_s->vibrato_counter + 1) & 0x1fff;

	//Update tremolo value/increment
	if ((vrc7_s->vibrato_counter & 0x3f) == 0) {
		vrc7_s->tremolo_value += vrc7_s->tremolo_inc;
		if (vrc7_s->tremolo_value >= 0x69 || vrc7_s->tremolo_value <= 0)
			vrc7_s->tremolo_inc *= -1;
	}
}

static void update_envelope_counters(struct vrc7_sound *vrc7_s){
	//Update envelope counters
	vrc7_s->mini_counter = (vrc7_s->mini_counter + 1) & 3;
	if (vrc7_s->mini_counter == 0)
		vrc7_s->envelope_counter++;

	//Update zero count
	vrc7_s->zero_count = 1;
	uint32_t envelope_copy = vrc7_s->envelope_counter;
	for (int i = 0; i < 13; i++) {
		if ((envelope_copy & 1) == 0) {
			vrc7_s->zero_count++;
			envelope_copy >>= 1;
		}
		else
			break;
	}
	if (vrc7_s->zero_count > 13)
		vrc7_s->zero_count = 0;
}

/*
==================================================
             VRC7 SOUND MANAGEMENT 
==================================================
*/

VRC7SOUND_API struct vrc7_sound *vrc7_new() {
	make_tables();

	struct vrc7_sound *vrc7_s = (struct vrc7_sound *) malloc(sizeof(struct vrc7_sound));

	for (int i = 0; i < VRC7_NUM_CHANNELS; i++) {
		vrc7_s->channels[i] = (struct vrc7_channel *) malloc(sizeof(struct vrc7_channel));
		vrc7_s->channels[i]->slots[MODULATOR] = (struct vrc7_slot *) malloc(sizeof(struct vrc7_slot));
		vrc7_s->channels[i]->slots[CARRIER] = (struct vrc7_slot *) malloc(sizeof(struct vrc7_slot));
	}

	for (int i = 0; i < VRC7_NUM_PATCHES; i++) {
		vrc7_s->patches[i] = (struct vrc7_patch *) malloc(sizeof(struct vrc7_patch));
	}

	vrc7_s->signal = malloc(VRC7_SIGNAL_CHUNK_LENGTH * sizeof(int16_t));

	vrc7_reset(vrc7_s);
	return vrc7_s;
}

VRC7SOUND_API void vrc7_delete(struct vrc7_sound *vrc7_s) {
	free(vrc7_s->signal);

	for (int i = 0; i < VRC7_NUM_PATCHES; i++) {
		free(vrc7_s->patches[i]);
	}

	for (int i = 0; i < VRC7_NUM_CHANNELS; i++) {
		free(vrc7_s->channels[i]->slots[CARRIER]);
		free(vrc7_s->channels[i]->slots[MODULATOR]);
		free(vrc7_s->channels[i]);
	}
	
	free(vrc7_s);
}

VRC7SOUND_API void vrc7_reset(struct vrc7_sound *vrc7_s) {
	vrc7_set_patch_set(vrc7_s, VRC7_NUKE_TONE);
	vrc7_set_clock_rate(vrc7_s, VRC7_DEFAULT_CLOCK_RATE);
	vrc7_set_sample_rate(vrc7_s, VRC7_DEFAULT_SAMPLE_RATE);
	vrc7_s->vibrato_counter = 0;
	vrc7_s->tremolo_value = 0;
	vrc7_s->tremolo_inc = 1;
	vrc7_s->envelope_counter = 0;
	vrc7_s->zero_count = 0;
	vrc7_s->mini_counter = 0;
	vrc7_s->address = 0x00;
	vrc7_s->channel_mask = (1 << VRC7_NUM_CHANNELS) - 1;

	for (int i = 0; i < VRC7_NUM_CHANNELS; i++) {
		vrc7_s->channels[i]->fNum = 0;
		vrc7_s->channels[i]->octave = 0;
		vrc7_s->channels[i]->volume = 0;
		vrc7_s->channels[i]->instrument = 0;
		vrc7_s->channels[i]->sustain = false;
		vrc7_s->channels[i]->trigger = false;

		for (int j = 0; j < 2; j++) {
			int type = j == 0 ? MODULATOR : CARRIER;
			vrc7_s->channels[i]->slots[type]->type = type;
			vrc7_s->channels[i]->slots[type]->sample = 0;
			vrc7_s->channels[i]->slots[type]->sample_prev = 0;
			vrc7_s->channels[i]->slots[type]->phase = 0;
			vrc7_s->channels[i]->slots[type]->env_stage = ENV_RELEASE;
			vrc7_s->channels[i]->slots[type]->env_value = 0x7f;
			vrc7_s->channels[i]->slots[type]->env_enabled = false;
			vrc7_s->channels[i]->slots[type]->restart_env = false;
		}
	}
}

VRC7SOUND_API void vrc7_clear(struct vrc7_sound *vrc7_s) {
	vrc7_s->tremolo_value = 0;
	vrc7_s->tremolo_inc = 1;
	//TODO
}

VRC7SOUND_API void vrc7_set_clock_rate(struct vrc7_sound *vrc7_s, double clock_rate) {
	vrc7_s->clock_rate = clock_rate;
	double alpha1 = 27000.0 + 33000.0;
	double alpha2 = 0.0047 * 27.0 * 33.0 * 2.0 * clock_rate; //0.0000000047*27000.0*33000.0*2.0*clock_rate;
	vrc7_s->fir_coeff = 33000.0/(alpha1+alpha2);
	vrc7_s->iir_coeff = -(alpha1 - alpha2) / (alpha1 + alpha2);
}

VRC7SOUND_API void vrc7_set_sample_rate(struct vrc7_sound *vrc7_s, double sample_rate) {
	vrc7_s->sample_rate = sample_rate;
	vrc7_s->sample_length = vrc7_s->clock_rate / sample_rate;
	vrc7_s->current_time = 0.0f;
}

VRC7SOUND_API void vrc7_set_patch_set(struct vrc7_sound *vrc7_s, int set) {
	for (int i = 0; i < VRC7_NUM_PATCHES; i++) {
		vrc7_get_default_patch(set, i, vrc7_s->patches[i]);
	}
	vrc7_s->patch_set = set;
}

VRC7SOUND_API void vrc7_tick(struct vrc7_sound *vrc7_s) {
	//Update channels
	for (int i = 0; i < 18; i++) {
		vrc7_s->signal[i * 4 + 0] = 0;
		vrc7_s->signal[i * 4 + 1] = 0;
		vrc7_s->signal[i * 4 + 2] = 0;
		vrc7_s->signal[i * 4 + 3] = 0;
		if (CHANNEL_SCHEDULE[i] < VRC7_NUM_CHANNELS) {
			int32_t val = update_slot(vrc7_s, CHANNEL_SCHEDULE[i], TYPE_SCHEDULE[i]);
			if (!BIT_TEST(vrc7_s->channel_mask, CHANNEL_SCHEDULE[i]) && TYPE_SCHEDULE[i] == CARRIER) {
				vrc7_s->signal[i * 4] = val >> 3;
			}
		}
#ifdef VRC7_TEST_REG
		if (!vrc7_s->test_counters) {
#endif
		  if (i == 16) {
		  	  update_fmam(vrc7_s);
			  update_envelope_counters(vrc7_s);
		  }
#ifdef VRC7_TEST_REG
		} else {	//Update
			update_fmam(vrc7_s);
		}

		if (vrc7_s->test_halt_phase) {
			if (CHANNEL_SCHEDULE[i] < VRC7_NUM_CHANNELS) {
				int channel = CHANNEL_SCHEDULE[i];
				int type = TYPE_SCHEDULE[i];
				vrc7_s->channels[channel]->slots[type]->phase = 0;
			}
		}
#endif
	}

	//Filter output
	static double prev_input = 0.0;
	static double prev_output = 0.0;
	for (int i = 0; i < VRC7_SIGNAL_CHUNK_LENGTH; i++) {
		double output =   prev_input*vrc7_s->fir_coeff
						+ vrc7_s->signal[i] * vrc7_s->fir_coeff
						+ prev_output * vrc7_s->iir_coeff;
		prev_input = vrc7_s->signal[i];
		prev_output = output;

		vrc7_s->signal[i] = (int16_t)(output * VRC7_AMPLIFIER_GAIN * 256);	//Arbitrary constant, but seems to fit
	}
}

VRC7SOUND_API int16_t vrc7_fetch_sample(struct vrc7_sound *vrc7_s) {
	while (vrc7_s->current_time >= VRC7_SIGNAL_CHUNK_LENGTH) {
		vrc7_tick(vrc7_s);
		vrc7_s->current_time -= VRC7_SIGNAL_CHUNK_LENGTH;
	}
	int16_t sample = vrc7_s->signal[(int) vrc7_s->current_time];
	vrc7_s->current_time += vrc7_s->sample_length;
	return sample;
}

/*
==================================================
                 VRC7 SOUND IO 
==================================================
*/

VRC7SOUND_API void vrc7_write_addr(struct vrc7_sound *vrc7_s, unsigned char addr) {
	vrc7_s->address = addr;
}

VRC7SOUND_API void vrc7_write_data(struct vrc7_sound *vrc7_s, unsigned char data) {
	int channel_num = vrc7_s->address & 0x0f;
	struct vrc7_patch *user_tone = vrc7_s->patches[0];

#ifdef VRC7_TEST_REG
	if (vrc7_s->test_counters) {
		if (BIT_TEST(data, 2))	//Not really the correct behaviour, but the best approximation since the emulator only updates once per sample
			vrc7_s->envelope_counter = 0xffff;
		else
			vrc7_s->envelope_counter = 0;
	}
#endif

	switch (vrc7_s->address) {
	case 0x00:
		user_tone->mult[MODULATOR] = data & 0x0f;
		user_tone->key_scale_rate[MODULATOR] = BIT_TEST(data, 4);
		user_tone->sustained[MODULATOR] = BIT_TEST(data, 5);
		user_tone->vibrato[MODULATOR] = BIT_TEST(data, 6);
		user_tone->tremolo[MODULATOR] = BIT_TEST(data, 7);
		break;
	case 0x01:
		user_tone->mult[CARRIER] = data & 0x0f;
		user_tone->key_scale_rate[CARRIER] = BIT_TEST(data, 4);
		user_tone->sustained[CARRIER] = BIT_TEST(data, 5);
		user_tone->vibrato[CARRIER] = BIT_TEST(data, 6);
		user_tone->tremolo[CARRIER] = BIT_TEST(data, 7);
		break;
	case 0x02:
		user_tone->total_level = data & 0x3f;
		user_tone->key_scale_level[MODULATOR] = data >> 6;
		break;
	case 0x03:
		user_tone->feedback = data & 0x07;
		user_tone->rect[MODULATOR] = BIT_TEST(data, 3);
		user_tone->rect[CARRIER] = BIT_TEST(data, 4);
		user_tone->key_scale_level[CARRIER] = data >> 6;
		break;
	case 0x04:
		user_tone->attack_rate[MODULATOR] = data >> 4;
		user_tone->decay_rate[MODULATOR] = data & 0x0f;
		break;
	case 0x05:
		user_tone->attack_rate[CARRIER] = data >> 4;
		user_tone->decay_rate[CARRIER] = data & 0x0f;
		break;
	case 0x06:
		user_tone->sustain_level[MODULATOR] = data >> 4;
		user_tone->release_rate[MODULATOR] = data & 0x0f;
		break;
	case 0x07:
		user_tone->sustain_level[CARRIER] = data >> 4;
		user_tone->release_rate[CARRIER] = data & 0x0f;
		break;
	case 0x0f:	//Test register
		vrc7_s->test_envelope = BIT_TEST(data, 0);
		//vrc7_s->test_noise = BIT_TEST(data, 1); 
		vrc7_s->test_halt_phase = BIT_TEST(data, 2);
		vrc7_s->test_counters = BIT_TEST(data, 3);
		break;
	default:
		if (channel_num >= VRC7_NUM_CHANNELS)
			return;

		struct vrc7_channel *channel = vrc7_s->channels[channel_num];

		if ((vrc7_s->address & 0xf0) == 0x10) {
			channel->fNum = (channel->fNum & 0x100) + data;
		}else if ((vrc7_s->address & 0xf0) == 0x20) {
			bool prev_trigger = channel->trigger;
			channel->fNum = (channel->fNum & 0xff) + ((data & 0x01) << 8);
			channel->octave = (data >> 1) & 0x07;
			channel->trigger = BIT_TEST(data, 4);
			if (channel->trigger && !prev_trigger) {
				channel->slots[MODULATOR]->restart_env = true;
				channel->slots[CARRIER]->restart_env = true;
			}
			channel->sustain = BIT_TEST(data, 5);
		}else if ((vrc7_s->address & 0xf0) == 0x30) {
			channel->volume = data & 0x0f;
			channel->instrument = data >> 4;
		}
	}
}

/*
==================================================
               VRC7 PATCH UTILITY
==================================================
*/

VRC7SOUND_API void vrc7_patch_to_reg(struct vrc7_patch *patch, unsigned char *reg) {
	unsigned char byte = patch->mult[MODULATOR];
	byte |= patch->key_scale_rate[MODULATOR] ? 0x10 : 0;
	byte |= patch->sustained[MODULATOR] ? 0x20 : 0;
	byte |= patch->vibrato[MODULATOR] ? 0x40 : 0;
	byte |= patch->tremolo[MODULATOR] ? 0x80 : 0;
	reg[0] = byte;

	byte = patch->mult[CARRIER];
	byte |= patch->key_scale_rate[CARRIER] ? 0x10 : 0;
	byte |= patch->sustained[CARRIER] ? 0x20 : 0;
	byte |= patch->vibrato[CARRIER] ? 0x40 : 0;
	byte |= patch->tremolo[CARRIER] ? 0x80 : 0;
	reg[1] = byte;

	byte = patch->total_level;
	byte |= patch->key_scale_level[MODULATOR] << 6;
	reg[2] = byte;

	byte = patch->feedback;
	byte |= patch->rect[MODULATOR] ? 0x08 : 0;
	byte |= patch->rect[CARRIER] ? 0x10 : 0;
	byte |= patch->key_scale_level[CARRIER] << 6;
	reg[3] = byte;

	byte = patch->decay_rate[MODULATOR];
	byte |= patch->attack_rate[MODULATOR] <<4;
	reg[4] = byte;

	byte = patch->decay_rate[CARRIER];
	byte |= patch->attack_rate[CARRIER] <<4;
	reg[5] = byte;

	byte = patch->release_rate[MODULATOR];
	byte |= patch->sustain_level[MODULATOR] <<4;
	reg[6] = byte;

	byte = patch->release_rate[CARRIER];
	byte |= patch->sustain_level[CARRIER] <<4;
	reg[7] = byte;
}

VRC7SOUND_API void vrc7_reg_to_patch(unsigned const char *reg, struct vrc7_patch *patch) {
	patch->total_level = reg[2] & 0x3f;
	patch->feedback = reg[3] & 0x07;

	patch->mult[MODULATOR] = reg[0] & 0x0f;
	patch->key_scale_rate[MODULATOR] = BIT_TEST(reg[0],4);
	patch->sustained[MODULATOR] = BIT_TEST(reg[0], 5);
	patch->vibrato[MODULATOR] = BIT_TEST(reg[0], 6);
	patch->tremolo[MODULATOR] = BIT_TEST(reg[0], 7);

	patch->key_scale_level[MODULATOR] = reg[2] >> 6;
	
	patch->rect[MODULATOR] = BIT_TEST(reg[3], 3);

	patch->attack_rate[MODULATOR] = reg[4] >> 4;
	patch->decay_rate[MODULATOR] = reg[4] & 0x0f;

	patch->sustain_level[MODULATOR] = reg[6] >> 4;
	patch->release_rate[MODULATOR] = reg[6] & 0x0f;

	patch->mult[CARRIER] = reg[1] & 0x0f;
	patch->key_scale_rate[CARRIER] = BIT_TEST(reg[1], 4);
	patch->sustained[CARRIER] = BIT_TEST(reg[1], 5);
	patch->vibrato[CARRIER] = BIT_TEST(reg[1], 6);
	patch->tremolo[CARRIER] = BIT_TEST(reg[1], 7);

	patch->rect[CARRIER] = BIT_TEST(reg[3], 4);
	patch->key_scale_level[CARRIER] = reg[3] >> 6;

	patch->attack_rate[CARRIER] = reg[5] >> 4;
	patch->decay_rate[CARRIER] = reg[5] & 0x0f;

	patch->sustain_level[CARRIER] = reg[7] >> 4;
	patch->release_rate[CARRIER] = reg[7] & 0x0f;
}

VRC7SOUND_API void vrc7_get_default_patch(int set, uint32_t index, struct vrc7_patch *patch) {
	unsigned const char *start = DEFAULT_INST[set] + index * 16 * sizeof(unsigned char);
	vrc7_reg_to_patch(start, patch);
}
