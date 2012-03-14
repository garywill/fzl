#include "eval.hpp"
#include "assert.hpp"
#include "mobility.hpp"
#include "util.hpp"
#include "pawn_structure_hash_table.hpp"
#include "tables.hpp"
#include "zobrist.hpp"

#include <iostream>
#include <cmath>

extern uint64_t const king_pawn_shield[2][64];
extern uint64_t const isolated_pawns[64];

eval_values_t eval_values;

eval_values_t::eval_values_t()
{
	// Untweaked values
	material_values[pieces::none] =      0;
	material_values[pieces::king] =  20000;
	king_attack_by_piece[pieces::none] = 0;
	king_check_by_piece[pieces::none] = 0;
	king_check_by_piece[pieces::pawn] = 0;
	hanging_piece[pieces::none] = 0;
	king_attack_min = 0;
	castled                     =     0;

	// Tweaked values
	material_values[1]          =    75;
	material_values[2]          =   313;
	material_values[3]          =   295;
	material_values[4]          =   568;
	material_values[5]          =   960;
	double_bishop               =    24;
	doubled_pawn[0]             =   -28;
	passed_pawn[0]              =     1;
	isolated_pawn[0]            =   -10;
	connected_pawn[0]           =     0;
	doubled_pawn[1]             =    -7;
	passed_pawn[1]              =     7;
	isolated_pawn[1]            =   -17;
	connected_pawn[1]           =     2;
	pawn_shield[0]              =    13;
	pawn_shield[1]              =     0;
	pin_absolute_bishop         =     1;
	pin_absolute_rook           =     3;
	pin_absolute_queen          =     1;
	mobility_scale[0]           =   115;
	mobility_scale[1]           =    88;
	pin_scale[0]                =    39;
	pin_scale[1]                =    16;
	rooks_on_open_file_scale    =     6;
	connected_rooks_scale[0]    =   141;
	connected_rooks_scale[1]    =     2;
	tropism_scale[0]            =    17;
	tropism_scale[1]            =     1;
	king_attack_by_piece[1]     =     2;
	king_attack_by_piece[2]     =    15;
	king_attack_by_piece[3]     =    14;
	king_attack_by_piece[4]     =     3;
	king_attack_by_piece[5]     =    12;
	king_check_by_piece[2]      =    12;
	king_check_by_piece[3]      =    13;
	king_check_by_piece[4]      =     9;
	king_check_by_piece[5]      =     1;
	king_melee_attack_by_rook   =     2;
	king_melee_attack_by_queen  =     9;
	king_attack_max             =   144;
	king_attack_rise            =     3;
	king_attack_exponent        =   214;
	king_attack_offset          =    48;
	king_attack_scale[0]        =    14;
	king_attack_scale[1]        =    16;
	center_control_scale[0]     =    31;
	center_control_scale[1]     =    13;
	material_imbalance_scale    =     4;
	rule_of_the_square          =    14;
	passed_pawn_unhindered      =     8;
	unstoppable_pawn_scale[0]   =    22;
	unstoppable_pawn_scale[1]   =    35;
	hanging_piece[1]            =     1;
	hanging_piece[2]            =     1;
	hanging_piece[3]            =     2;
	hanging_piece[4]            =     1;
	hanging_piece[5]            =     1;
	hanging_piece_scale[0]      =    79;
	hanging_piece_scale[1]      =   137;
	mobility_knight_min         =   -18;
	mobility_knight_max         =    44;
	mobility_knight_rise        =     1;
	mobility_knight_offset      =     2;
	mobility_bishop_min         =   -11;
	mobility_bishop_max         =    23;
	mobility_bishop_rise        =     1;
	mobility_bishop_offset      =     6;
	mobility_rook_min           =   -48;
	mobility_rook_max           =    23;
	mobility_rook_rise          =     1;
	mobility_rook_offset        =     2;
	mobility_queen_min          =   -36;
	mobility_queen_max          =    19;
	mobility_queen_rise         =     1;
	mobility_queen_offset       =    12;

	update_derived();
}

void eval_values_t::update_derived()
{
	initial_material = 
		material_values[pieces::pawn] * 8 + 
		material_values[pieces::knight] * 2 +
		material_values[pieces::bishop] * 2 +
		material_values[pieces::rook] * 2 +
		material_values[pieces::queen];

	//phase_transition_material_begin = initial_material * 2; - phase_transition_begin;
	//phase_transition_material_end = phase_transition_material_begin - phase_transition_duration;
	phase_transition_material_begin = initial_material * 2;
	phase_transition_material_end = 1000;
	phase_transition_begin = 0;
	phase_transition_duration = phase_transition_material_begin - phase_transition_material_end;

	for( short i = 0; i < 8; ++i ) {
		if( i > mobility_knight_offset ) {
			mobility_knight[i] = (std::min)(short(mobility_knight_min + mobility_knight_rise * (i - mobility_knight_offset)), mobility_knight_max);
		}
		else {
			mobility_knight[i] = mobility_knight_min;
		}
	}

	for( short i = 0; i < 13; ++i ) {
		if( i > mobility_bishop_offset ) {
			mobility_bishop[i] = (std::min)(short(mobility_bishop_min + mobility_bishop_rise * (i - mobility_bishop_offset)), mobility_bishop_max);
		}
		else {
			mobility_bishop[i] = mobility_bishop_min;
		}
	}

	for( short i = 0; i < 14; ++i ) {
		if( i > mobility_rook_offset ) {
			mobility_rook[i] = (std::min)(short(mobility_rook_min + mobility_rook_rise * (i - mobility_rook_offset)), mobility_rook_max);
		}
		else {
			mobility_rook[i] = mobility_rook_min;
		}
	}

	for( short i = 0; i < 27; ++i ) {
		if( i > mobility_queen_offset ) {
			mobility_queen[i] = (std::min)(short(mobility_queen_min + mobility_queen_rise * (i - mobility_queen_offset)), mobility_queen_max);
		}
		else {
			mobility_queen[i] = mobility_queen_min;
		}
	}

	for( short i = 0; i < 150; ++i ) {
		if( i > king_attack_offset ) {
			double factor = i - king_attack_offset;
			factor = std::pow( factor, double(eval_values.king_attack_exponent) / 100.0 );
			king_attack[i] = (std::min)(short(king_attack_min + king_attack_rise * factor), king_attack_max);
		}
		else {
			king_attack[i] = king_attack_min;
		}
	}
}


short phase_scale( short const* material, short ev1, short ev2 )
{
	int m = material[0] + material[1];
	if( m >= eval_values.phase_transition_material_begin ) {
		return ev1;
	}
	else if( m <= eval_values.phase_transition_material_end ) {
		return ev2;
	}
	
	int position = 256 * (eval_values.phase_transition_material_begin - m) / static_cast<int>(eval_values.phase_transition_duration);
	return ((static_cast<int>(ev1) * position      ) /256) +
		   ((static_cast<int>(ev2) * (256-position)) /256);
}


extern short const pawn_values[2][64] = {
	{
		 0,  0,  0,   0,   0,  0,  0,  0,
		 0,  0,  0, -20, -20,  0,  0,  0,
		 2,  2,  2,   5,   5,  2,  2,  2,
		 3,  5, 10,  20,  20, 10,  5,  3,
		 6,  8, 17,  27,  27, 17,  8,  6,
		10, 15, 25,  40,  40, 25, 15, 10,
		45, 50, 55,  60,  60, 55, 50, 45,
		 0,  0,  0,   0,   0,  0,  0,  0
	},
	{
		 0,  0,  0,   0,   0,  0,  0,  0,
		45, 50, 55,  60,  60, 55, 50, 45,
		10, 15, 25,  40,  40, 25, 15, 10,
		 6,  8, 17,  27,  27, 17,  8,  6,
		 3,  5, 10,  20,  20, 10,  5,  3,
		 2,  2,  2,   5,   5,  2,  2,  2,
		 0,  0,  0, -20, -20,  0,  0,  0,
		 0,  0,  0,   0,   0,  0,  0,  0
	}
};


extern short const queen_values[2][64] = {
	{
		 -20,  -10,  -10,  -5 ,  -5 ,  -10,  -10,  -20,
		 -10,    0,   2 ,   3 ,   3 ,   2 , 0   ,  -10,
		 -10,    2,   5 ,   5 ,   5 ,   5 ,   2 ,  -10,
		  -5,    3,   5 ,   7 ,   7 ,   5 ,   3 ,  -5 ,
		  -5,    3,   5 ,   7 ,   7 ,   5 ,   3 ,  -5 ,
		 -10,    2,   5 ,   5 ,   5 ,   5 ,   2 ,  -10,
		 -10, 0   ,   2 ,   3 ,   3 ,   2 , 0   ,  -10,
		 -20,  -10,  -10,  -5 ,  -5 ,  -10,  -10,  -20,
	},
	{
		 -20,  -10,  -10,  -5 ,  -5 ,  -10,  -10,  -20,
		 -10, 0   ,   2 ,   3 ,   3 ,   2 , 0   ,  -10,
		 -10,   2 ,   5 ,   5 ,   5 ,   5 ,   2 ,  -10,
		 -5 ,   3 ,   5 ,   7 ,   7 ,   5 ,   3 ,  -5 ,
		 -5 ,   3 ,   5 ,   7 ,   7 ,   5 ,   3 ,  -5 ,
		 -10,   2 ,   5 ,   5 ,   5 ,   5 ,   2 ,  -10,
		 -10, 0   ,   2 ,   3 ,   3 ,   2 , 0   ,  -10,
		 -20,  -10,  -10,  -5 ,  -5 ,  -10,  -10,  -20,
	}
};

extern short const rook_values[2][64] = {
	{
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		 5, 5, 5, 5, 5, 5, 5,  5,
		 0, 0, 0, 0, 0, 0, 0,  0
	},
	{
		 0, 0, 0, 0, 0, 0, 0,  0,
		 5, 5, 5, 5, 5, 5, 5,  5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
		-5, 0, 0, 0, 0, 0, 0, -5,
	}
};

extern short const knight_values[2][64] = {
	{
		 -25,  -15,  -10,  -10,  -10,  -10,  -15,  -25,
		 -15,  -5,  0,      2,    2,  0,     -5,   -15,
		 -10, 0,      2,    5,    5,    2,  0,     -10,
		 -10,   2,    5,    10,   10,   5,    2,   -10,
		 -10,   2,    5,    10,   10,   5,    2,   -10,
		 -10, 0,      2,    5,    5,    2,  0,     -10,
		 -15,  -5,  0,      2,    2,  0,     -5,   -15,
		 -25,  -15,  -10,  -10,  -10,  -10,  -15,  -25
	},
	{
		 -25,  -15,  -10,  -10,  -10,  -10,  -15,  -25,
		 -15,  -5,  0,      2,    2,  0,     -5,   -15,
		 -10, 0,      2,    5,    5,    2,  0,     -10,
		 -10,   2,    5,    10,   10,   5,    2,   -10,
		 -10,   2,    5,    10,   10,   5,    2,   -10,
		 -10, 0,      2,    5,    5,    2,  0,     -10,
		 -15,  -5,  0,      2,    2,  0,     -5,   -15,
		 -25,  -15,  -10,  -10,  -10,  -10,  -15,  -25
	}
};

extern short const bishop_values[2][64] = {
	{
		 -10,  -9,   -7,   -5,   -5,   -7,   -9,   -10,
		 -2,    10,   5,    10,   10,   5,    10,  -2,
		  3,    6,    15,   12,   12,   15,   6,    3,
		  5,    10,   12,   20,   20,   12,   10,   5,
		  5,    10,   12,   20,   20,   12,   10,   5,
		  3,    6,    15,   12,   12,   15,   6,    3,
		0,      10,   5,    10,   10,   5,    10, 0,
		0,    0,      2,    5,    5,    2,  0,    0
	},
	{
		0,    0,      2,    5,    5,    2,  0,    0,
		0,      10,   5,    10,   10,   5,    10, 0,
		  3,    6,    15,   12,   12,   15,   6,    3,
		  5,    10,   12,   20,   20,   12,   10,   5,
		  5,    10,   12,   20,   20,   12,   10,   5,
		  3,    6,    15,   12,   12,   15,   6,    3,
		 -2,    10,   5,    10,   10,   5,    10,  -2,
		 -10,  -9,   -7,   -5,   -5,   -7,   -9,   -10
	}
};

extern uint64_t const passed_pawns[2][64] = {
{
		0x0303030303030300ull,
		0x0707070707070700ull,
		0x0e0e0e0e0e0e0e00ull,
		0x1c1c1c1c1c1c1c00ull,
		0x3838383838383800ull,
		0x7070707070707000ull,
		0xe0e0e0e0e0e0e000ull,
		0xc0c0c0c0c0c0c000ull,
		0x0303030303030000ull,
		0x0707070707070000ull,
		0x0e0e0e0e0e0e0000ull,
		0x1c1c1c1c1c1c0000ull,
		0x3838383838380000ull,
		0x7070707070700000ull,
		0xe0e0e0e0e0e00000ull,
		0xc0c0c0c0c0c00000ull,
		0x0303030303000000ull,
		0x0707070707000000ull,
		0x0e0e0e0e0e000000ull,
		0x1c1c1c1c1c000000ull,
		0x3838383838000000ull,
		0x7070707070000000ull,
		0xe0e0e0e0e0000000ull,
		0xc0c0c0c0c0000000ull,
		0x0303030300000000ull,
		0x0707070700000000ull,
		0x0e0e0e0e00000000ull,
		0x1c1c1c1c00000000ull,
		0x3838383800000000ull,
		0x7070707000000000ull,
		0xe0e0e0e000000000ull,
		0xc0c0c0c000000000ull,
		0x0303030000000000ull,
		0x0707070000000000ull,
		0x0e0e0e0000000000ull,
		0x1c1c1c0000000000ull,
		0x3838380000000000ull,
		0x7070700000000000ull,
		0xe0e0e00000000000ull,
		0xc0c0c00000000000ull,
		0x0303000000000000ull,
		0x0707000000000000ull,
		0x0e0e000000000000ull,
		0x1c1c000000000000ull,
		0x3838000000000000ull,
		0x7070000000000000ull,
		0xe0e0000000000000ull,
		0xc0c0000000000000ull,
		0x0300000000000000ull,
		0x0700000000000000ull,
		0x0e00000000000000ull,
		0x1c00000000000000ull,
		0x3800000000000000ull,
		0x7000000000000000ull,
		0xe000000000000000ull,
		0xc000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull
	},
{
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000003ull,
		0x0000000000000007ull,
		0x000000000000000eull,
		0x000000000000001cull,
		0x0000000000000038ull,
		0x0000000000000070ull,
		0x00000000000000e0ull,
		0x00000000000000c0ull,
		0x0000000000000303ull,
		0x0000000000000707ull,
		0x0000000000000e0eull,
		0x0000000000001c1cull,
		0x0000000000003838ull,
		0x0000000000007070ull,
		0x000000000000e0e0ull,
		0x000000000000c0c0ull,
		0x0000000000030303ull,
		0x0000000000070707ull,
		0x00000000000e0e0eull,
		0x00000000001c1c1cull,
		0x0000000000383838ull,
		0x0000000000707070ull,
		0x0000000000e0e0e0ull,
		0x0000000000c0c0c0ull,
		0x0000000003030303ull,
		0x0000000007070707ull,
		0x000000000e0e0e0eull,
		0x000000001c1c1c1cull,
		0x0000000038383838ull,
		0x0000000070707070ull,
		0x00000000e0e0e0e0ull,
		0x00000000c0c0c0c0ull,
		0x0000000303030303ull,
		0x0000000707070707ull,
		0x0000000e0e0e0e0eull,
		0x0000001c1c1c1c1cull,
		0x0000003838383838ull,
		0x0000007070707070ull,
		0x000000e0e0e0e0e0ull,
		0x000000c0c0c0c0c0ull,
		0x0000030303030303ull,
		0x0000070707070707ull,
		0x00000e0e0e0e0e0eull,
		0x00001c1c1c1c1c1cull,
		0x0000383838383838ull,
		0x0000707070707070ull,
		0x0000e0e0e0e0e0e0ull,
		0x0000c0c0c0c0c0c0ull,
		0x0003030303030303ull,
		0x0007070707070707ull,
		0x000e0e0e0e0e0e0eull,
		0x001c1c1c1c1c1c1cull,
		0x0038383838383838ull,
		0x0070707070707070ull,
		0x00e0e0e0e0e0e0e0ull,
		0x00c0c0c0c0c0c0c0ull
	}
};

extern uint64_t const doubled_pawns[2][64] = {
	{
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000001ull,
		0x0000000000000002ull,
		0x0000000000000004ull,
		0x0000000000000008ull,
		0x0000000000000010ull,
		0x0000000000000020ull,
		0x0000000000000040ull,
		0x0000000000000080ull,
		0x0000000000000101ull,
		0x0000000000000202ull,
		0x0000000000000404ull,
		0x0000000000000808ull,
		0x0000000000001010ull,
		0x0000000000002020ull,
		0x0000000000004040ull,
		0x0000000000008080ull,
		0x0000000000010101ull,
		0x0000000000020202ull,
		0x0000000000040404ull,
		0x0000000000080808ull,
		0x0000000000101010ull,
		0x0000000000202020ull,
		0x0000000000404040ull,
		0x0000000000808080ull,
		0x0000000001010101ull,
		0x0000000002020202ull,
		0x0000000004040404ull,
		0x0000000008080808ull,
		0x0000000010101010ull,
		0x0000000020202020ull,
		0x0000000040404040ull,
		0x0000000080808080ull,
		0x0000000101010101ull,
		0x0000000202020202ull,
		0x0000000404040404ull,
		0x0000000808080808ull,
		0x0000001010101010ull,
		0x0000002020202020ull,
		0x0000004040404040ull,
		0x0000008080808080ull,
		0x0000010101010101ull,
		0x0000020202020202ull,
		0x0000040404040404ull,
		0x0000080808080808ull,
		0x0000101010101010ull,
		0x0000202020202020ull,
		0x0000404040404040ull,
		0x0000808080808080ull,
		0x0001010101010101ull,
		0x0002020202020202ull,
		0x0004040404040404ull,
		0x0008080808080808ull,
		0x0010101010101010ull,
		0x0020202020202020ull,
		0x0040404040404040ull,
		0x0080808080808080ull
	},
	{
		0x0101010101010100ull,
		0x0202020202020200ull,
		0x0404040404040400ull,
		0x0808080808080800ull,
		0x1010101010101000ull,
		0x2020202020202000ull,
		0x4040404040404000ull,
		0x8080808080808000ull,
		0x0101010101010000ull,
		0x0202020202020000ull,
		0x0404040404040000ull,
		0x0808080808080000ull,
		0x1010101010100000ull,
		0x2020202020200000ull,
		0x4040404040400000ull,
		0x8080808080800000ull,
		0x0101010101000000ull,
		0x0202020202000000ull,
		0x0404040404000000ull,
		0x0808080808000000ull,
		0x1010101010000000ull,
		0x2020202020000000ull,
		0x4040404040000000ull,
		0x8080808080000000ull,
		0x0101010100000000ull,
		0x0202020200000000ull,
		0x0404040400000000ull,
		0x0808080800000000ull,
		0x1010101000000000ull,
		0x2020202000000000ull,
		0x4040404000000000ull,
		0x8080808000000000ull,
		0x0101010000000000ull,
		0x0202020000000000ull,
		0x0404040000000000ull,
		0x0808080000000000ull,
		0x1010100000000000ull,
		0x2020200000000000ull,
		0x4040400000000000ull,
		0x8080800000000000ull,
		0x0101000000000000ull,
		0x0202000000000000ull,
		0x0404000000000000ull,
		0x0808000000000000ull,
		0x1010000000000000ull,
		0x2020000000000000ull,
		0x4040000000000000ull,
		0x8080000000000000ull,
		0x0100000000000000ull,
		0x0200000000000000ull,
		0x0400000000000000ull,
		0x0800000000000000ull,
		0x1000000000000000ull,
		0x2000000000000000ull,
		0x4000000000000000ull,
		0x8000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull,
		0x0000000000000000ull
	}
};

namespace {
uint64_t const connected_pawns[2][64] = {
{
		0x0000000000000202ull,
		0x0000000000000505ull,
		0x0000000000000a0aull,
		0x0000000000001414ull,
		0x0000000000002828ull,
		0x0000000000005050ull,
		0x000000000000a0a0ull,
		0x0000000000004040ull,
		0x0000000000020202ull,
		0x0000000000050505ull,
		0x00000000000a0a0aull,
		0x0000000000141414ull,
		0x0000000000282828ull,
		0x0000000000505050ull,
		0x0000000000a0a0a0ull,
		0x0000000000404040ull,
		0x0000000002020200ull,
		0x0000000005050500ull,
		0x000000000a0a0a00ull,
		0x0000000014141400ull,
		0x0000000028282800ull,
		0x0000000050505000ull,
		0x00000000a0a0a000ull,
		0x0000000040404000ull,
		0x0000000202020000ull,
		0x0000000505050000ull,
		0x0000000a0a0a0000ull,
		0x0000001414140000ull,
		0x0000002828280000ull,
		0x0000005050500000ull,
		0x000000a0a0a00000ull,
		0x0000004040400000ull,
		0x0000020202000000ull,
		0x0000050505000000ull,
		0x00000a0a0a000000ull,
		0x0000141414000000ull,
		0x0000282828000000ull,
		0x0000505050000000ull,
		0x0000a0a0a0000000ull,
		0x0000404040000000ull,
		0x0002020200000000ull,
		0x0005050500000000ull,
		0x000a0a0a00000000ull,
		0x0014141400000000ull,
		0x0028282800000000ull,
		0x0050505000000000ull,
		0x00a0a0a000000000ull,
		0x0040404000000000ull,
		0x0202020000000000ull,
		0x0505050000000000ull,
		0x0a0a0a0000000000ull,
		0x1414140000000000ull,
		0x2828280000000000ull,
		0x5050500000000000ull,
		0xa0a0a00000000000ull,
		0x4040400000000000ull,
		0x0202000000000000ull,
		0x0505000000000000ull,
		0x0a0a000000000000ull,
		0x1414000000000000ull,
		0x2828000000000000ull,
		0x5050000000000000ull,
		0xa0a0000000000000ull,
		0x4040000000000000ull
	},
{
		0x0000000000000202ull,
		0x0000000000000505ull,
		0x0000000000000a0aull,
		0x0000000000001414ull,
		0x0000000000002828ull,
		0x0000000000005050ull,
		0x000000000000a0a0ull,
		0x0000000000004040ull,
		0x0000000000020202ull,
		0x0000000000050505ull,
		0x00000000000a0a0aull,
		0x0000000000141414ull,
		0x0000000000282828ull,
		0x0000000000505050ull,
		0x0000000000a0a0a0ull,
		0x0000000000404040ull,
		0x0000000002020200ull,
		0x0000000005050500ull,
		0x000000000a0a0a00ull,
		0x0000000014141400ull,
		0x0000000028282800ull,
		0x0000000050505000ull,
		0x00000000a0a0a000ull,
		0x0000000040404000ull,
		0x0000000202020000ull,
		0x0000000505050000ull,
		0x0000000a0a0a0000ull,
		0x0000001414140000ull,
		0x0000002828280000ull,
		0x0000005050500000ull,
		0x000000a0a0a00000ull,
		0x0000004040400000ull,
		0x0000020202000000ull,
		0x0000050505000000ull,
		0x00000a0a0a000000ull,
		0x0000141414000000ull,
		0x0000282828000000ull,
		0x0000505050000000ull,
		0x0000a0a0a0000000ull,
		0x0000404040000000ull,
		0x0002020200000000ull,
		0x0005050500000000ull,
		0x000a0a0a00000000ull,
		0x0014141400000000ull,
		0x0028282800000000ull,
		0x0050505000000000ull,
		0x00a0a0a000000000ull,
		0x0040404000000000ull,
		0x0202020000000000ull,
		0x0505050000000000ull,
		0x0a0a0a0000000000ull,
		0x1414140000000000ull,
		0x2828280000000000ull,
		0x5050500000000000ull,
		0xa0a0a00000000000ull,
		0x4040400000000000ull,
		0x0202000000000000ull,
		0x0505000000000000ull,
		0x0a0a000000000000ull,
		0x1414000000000000ull,
		0x2828000000000000ull,
		0x5050000000000000ull,
		0xa0a0000000000000ull,
		0x4040000000000000ull
	}
};


void evaluate_pawn( uint64_t own_pawns, uint64_t foreign_pawns, color::type c, uint64_t pawn,
					 uint64_t& unpassed, uint64_t& doubled, uint64_t& connected, uint64_t& unisolated )
{
	doubled |= doubled_pawns[c][pawn] & own_pawns;
	unpassed |= passed_pawns[c][pawn] & foreign_pawns;
	connected |= connected_pawns[c][pawn] & own_pawns;
	unisolated |= isolated_pawns[pawn] & own_pawns;
}
}

void evaluate_pawns( uint64_t white_pawns, uint64_t black_pawns, short* eval, uint64_t& passed )
{
	// Two while loops, otherwise nice branchless solution.

	uint64_t unpassed_white = 0;
	uint64_t doubled_white = 0;
	uint64_t connected_white = 0;
	uint64_t unisolated_white = 0;
	uint64_t unpassed_black = 0;
	uint64_t doubled_black = 0;
	uint64_t connected_black = 0;
	uint64_t unisolated_black = 0;
	{

		uint64_t pawns = white_pawns;
		while( pawns ) {
			uint64_t pawn = bitscan_unset( pawns );

			evaluate_pawn( white_pawns, black_pawns, color::white, pawn,
								  unpassed_black, doubled_white, connected_white, unisolated_white );

		}
	}


	{
		uint64_t pawns = black_pawns;
		while( pawns ) {
			uint64_t pawn = bitscan_unset( pawns );

			evaluate_pawn( black_pawns, white_pawns, color::black, pawn,
						   unpassed_white, doubled_black, connected_black, unisolated_black );

		}
	}
	unpassed_white |= doubled_white;
	unpassed_black |= doubled_black;

	uint64_t passed_white = white_pawns ^ unpassed_white;
	uint64_t passed_black = black_pawns ^ unpassed_black;
	passed = passed_white | passed_black;

	eval[0] = static_cast<short>(eval_values.passed_pawn[0] * popcount(passed_white));
	eval[0] += static_cast<short>(eval_values.doubled_pawn[0] * popcount(doubled_white));
	eval[0] += static_cast<short>(eval_values.connected_pawn[0] * popcount(connected_white));
	eval[0] += static_cast<short>(eval_values.isolated_pawn[0] * popcount(white_pawns ^ unisolated_white));
	eval[0] -= static_cast<short>(eval_values.passed_pawn[0] * popcount(passed_black));
	eval[0] -= static_cast<short>(eval_values.doubled_pawn[0] * popcount(doubled_black));
	eval[0] -= static_cast<short>(eval_values.connected_pawn[0] * popcount(connected_black));
	eval[0] -= static_cast<short>(eval_values.isolated_pawn[0] * popcount(black_pawns ^ unisolated_black));

	eval[1] = static_cast<short>(eval_values.passed_pawn[1] * popcount(passed_white));
	eval[1] += static_cast<short>(eval_values.doubled_pawn[1] * popcount(doubled_white));
	eval[1] += static_cast<short>(eval_values.connected_pawn[1] * popcount(connected_white));
	eval[1] += static_cast<short>(eval_values.isolated_pawn[1] * popcount(white_pawns ^ unisolated_white));
	eval[1] -= static_cast<short>(eval_values.passed_pawn[1] * popcount(passed_black));
	eval[1] -= static_cast<short>(eval_values.doubled_pawn[1] * popcount(doubled_black));
	eval[1] -= static_cast<short>(eval_values.connected_pawn[1] * popcount(connected_black));
	eval[1] -= static_cast<short>(eval_values.isolated_pawn[1] * popcount(black_pawns ^ unisolated_black));
}


short evaluate_side( position const& p, color::type c )
{
	short result = 0;

	uint64_t all_pieces = p.bitboards[c].b[bb_type::all_pieces];
	while( all_pieces ) {
		uint64_t piece = bitscan_unset( all_pieces );

		uint64_t bpiece = 1ull << piece;
		if( p.bitboards[c].b[bb_type::pawns] & bpiece ) {
			result += pawn_values[c][piece] + eval_values.material_values[pieces::pawn];
		}
		else if( p.bitboards[c].b[bb_type::knights] & bpiece ) {
			result += knight_values[c][piece] + eval_values.material_values[pieces::knight];
		}
		else if( p.bitboards[c].b[bb_type::bishops] & bpiece ) {
			result += bishop_values[c][piece] + eval_values.material_values[pieces::bishop];
		}
		else if( p.bitboards[c].b[bb_type::rooks] & bpiece ) {
			result += rook_values[c][piece] + eval_values.material_values[pieces::rook];
		}
		else if( p.bitboards[c].b[bb_type::queens] & bpiece ) {
			result += queen_values[c][piece] + eval_values.material_values[pieces::queen];
		}
	}

	if( popcount( p.bitboards[c].b[bb_type::bishops] ) >= 2 ) {
		result += eval_values.double_bishop;
	}

	if( p.castle[c] & castles::has_castled ) {
		result += eval_values.castled;
	}

	return result;
}

short evaluate_fast( position const& p, color::type c )
{
	int value = evaluate_side( p, c ) - evaluate_side( p, static_cast<color::type>(1-c) );

	short pawn_eval[2];
	uint64_t tmp;
	evaluate_pawns( p.bitboards[0].b[bb_type::pawns], p.bitboards[1].b[bb_type::pawns], pawn_eval, tmp );
	short scaled_pawns = phase_scale( p.material, pawn_eval[0], pawn_eval[1] );
	if( c ) {
		value -= scaled_pawns;
	}
	else {
		value += scaled_pawns;
	}

	ASSERT( scaled_pawns == p.pawns.eval );
	ASSERT( value > result::loss && value < result::win );

	return value;
}

namespace {
static short get_piece_square_value( color::type c, pieces::type target, unsigned char pi )
{
	switch( target ) {
	case pieces::pawn:
		return pawn_values[c][pi] + eval_values.material_values[pieces::pawn];
	case pieces::knight:
		return knight_values[c][pi] + eval_values.material_values[pieces::knight];
	case pieces::bishop:
		return bishop_values[c][pi] + eval_values.material_values[pieces::bishop];
	case pieces::rook:
		return rook_values[c][pi] + eval_values.material_values[pieces::rook];
	case pieces::queen:
		return queen_values[c][pi] + eval_values.material_values[pieces::queen];
	default:
	case pieces::king:
		return 0;
	}
}
}


short evaluate_move( position const& p, color::type c, short current_evaluation, move const& m, position::pawn_structure& outPawns )
{
#if 0
	short old_eval = current_evaluation;
	{
		short fresh_eval = evaluate_fast( p, c );
		if( fresh_eval != current_evaluation ) {
			std::cerr << "BAD" << std::endl;
			abort();
		}
	}
#endif

	if( m.flags & move_flags::castle ) {
		
		current_evaluation += eval_values.castled;
		unsigned char row = c ? 56 : 0;
		if( m.target % 8 == 6 ) {
			// Kingside
			current_evaluation -= rook_values[c][7 + row];
			current_evaluation += rook_values[c][5 + row];
		}
		else {
			// Queenside
			current_evaluation -= rook_values[c][0 + row];
			current_evaluation += rook_values[c][3 + row];
		}

		outPawns = p.pawns;

		return current_evaluation;
	}

	if( m.captured_piece != pieces::none ) {
		if( m.flags & move_flags::enpassant ) {
			unsigned char ep = (m.target & 0x7) | (m.source & 0x38);
			current_evaluation += pawn_values[1-c][ep] + eval_values.material_values[pieces::pawn];
		}
		else {
			current_evaluation += get_piece_square_value( static_cast<color::type>(1-c), m.captured_piece, m.target );
		}

		if( m.captured_piece == pieces::bishop && popcount( p.bitboards[1-c].b[bb_type::bishops] ) == 2 ) {
			current_evaluation += eval_values.double_bishop;
		}
	}

	int promotion = m.flags & move_flags::promotion_mask;
	if( promotion ) {
		current_evaluation -= pawn_values[c][m.source] + eval_values.material_values[pieces::pawn];
		switch( promotion ) {
			case move_flags::promotion_knight:
				current_evaluation += knight_values[c][m.target];
				break;
			case move_flags::promotion_bishop:
				current_evaluation += bishop_values[c][m.target];
				if( popcount( p.bitboards[c].b[bb_type::bishops] ) == 1 ) {
					current_evaluation += eval_values.double_bishop;
				}
				break;
			case move_flags::promotion_rook:
				current_evaluation += rook_values[c][m.target];
				break;
			case move_flags::promotion_queen:
				current_evaluation += queen_values[c][m.target];
				break;
		}
		current_evaluation += eval_values.material_values[promotion >> move_flags::promotion_shift];
	}
	else {
		current_evaluation -= get_piece_square_value( c, m.piece, m.source );
		current_evaluation += get_piece_square_value( c, m.piece, m.target );
	}

	outPawns = p.pawns;
	if( m.piece == pieces::pawn || m.captured_piece != pieces::none ) {
		short material[2];
		material[0] = p.material[0];
		material[1] = p.material[1];

		if( m.captured_piece != pieces::none ) {
			material[1-c] -= get_material_value( m.captured_piece );
		}
		int promotion = m.flags & move_flags::promotion_mask;
		if( promotion ) {
			material[c] -= get_material_value( pieces::pawn );
			material[c] += get_material_value( static_cast<pieces::type>(promotion >> move_flags::promotion_shift) );
		}

		uint64_t pawnMap[2];
		pawnMap[0] = p.bitboards[0].b[bb_type::pawns];
		pawnMap[1] = p.bitboards[1].b[bb_type::pawns];

		if( m.captured_piece == pieces::pawn ) {
			if( m.flags & move_flags::enpassant ) {
				unsigned char ep = (m.target & 0x7) | (m.source & 0x38);
				pawnMap[1-c] &= ~(1ull << ep );
				outPawns.hash ^= get_pawn_structure_hash( static_cast<color::type>(1-c), ep );
			}
			else {
				pawnMap[1-c] &= ~(1ull << m.target );
				outPawns.hash ^= get_pawn_structure_hash( static_cast<color::type>(1-c), m.target );
			}
		}
		if( m.piece == pieces::pawn ) {
			pawnMap[c] &= ~(1ull << m.source );
			outPawns.hash ^= get_pawn_structure_hash( static_cast<color::type>(c), m.source );
			if( !promotion ) {
				outPawns.hash ^= get_pawn_structure_hash( static_cast<color::type>(c), m.target );
				pawnMap[c] |= 1ull << m.target;
			}
		}

		short pawn_eval[2];
		if( !pawn_hash_table.lookup( outPawns.hash, pawn_eval, outPawns.passed ) ) {
			evaluate_pawns(pawnMap[0], pawnMap[1], pawn_eval, outPawns.passed );
			pawn_hash_table.store( outPawns.hash, pawn_eval, outPawns.passed );
		}

		short scaled_eval = phase_scale( material, pawn_eval[0], pawn_eval[1] );
		if( c == color::white ) {
			current_evaluation -= outPawns.eval;
			current_evaluation += scaled_eval;
		}
		else {
			current_evaluation += outPawns.eval;
			current_evaluation -= scaled_eval;
		}
		outPawns.eval = scaled_eval;
	}

#if 0
	{
		position p2 = p;
		p2.init_pawn_structure();
		ASSERT( p.pawns.eval == p2.pawns.eval );
		ASSERT( p.pawns.hash == p2.pawns.hash );

		position p4 = p;
		p4.init_pawn_structure();

		apply_move( p2, m, c );
		short fresh_eval = evaluate_fast( p2, c );
		if( fresh_eval != current_evaluation ) {
			fresh_eval = evaluate_fast( p2, c );
			std::cerr << "BAD" << std::endl;

			position p3 = p;
			apply_move( p3, m, c );

			evaluate_move( p, c, evaluate_fast( p, c ), m, outPawns );
		}
	}
#endif

	return current_evaluation;
}


/* Pawn shield for king
 * 1 2 3
 * 4 5 6
 *   K
 * Pawns on 1, 3, 4, 6 are awarded shield_const points, pawns on 2 and 5 are awarded twice as many points.
 * Double pawns do not count.
 *
 * Special case: a and h file. There b and g also count twice.
 */
static void evaluate_pawn_shield_side( position const& p, color::type c, short* pawn_shield )
{
	uint64_t kings = p.bitboards[c].b[bb_type::king];
	uint64_t king = bitscan( kings );

	uint64_t shield = king_pawn_shield[c][king] & p.bitboards[c].b[bb_type::pawns];
	pawn_shield[0] = static_cast<short>(eval_values.pawn_shield[0] * popcount(shield));
	pawn_shield[1] = static_cast<short>(eval_values.pawn_shield[1] * popcount(shield));
}


void evaluate_pawn_shield( position const& p, color::type c, short* pawn_shield )
{
	short own[2];
	short other[2];

	evaluate_pawn_shield_side( p, c, own );
	evaluate_pawn_shield_side( p, static_cast<color::type>(1-c), other );

	pawn_shield[0] = own[0] - other[0];
	pawn_shield[1] = own[1] - other[1];
}


short evaluate_full( position const& p, color::type c )
{
	short eval = evaluate_fast( p, c );

	return evaluate_full( p, c, eval );
}

short evaluate_full( position const& p, color::type c, short eval_fast )
{
	short pawn_shield[2];
	evaluate_pawn_shield( p, c, pawn_shield );

	eval_fast += phase_scale( p.material, pawn_shield[0], pawn_shield[1] );
	
	eval_fast += evaluate_mobility( p, c );
	
	// Adjust score based on material. The basic idea is that,
	// given two positions with equal, non-zero score,
	// the position having fewer material is better.
	short v = 25 * (std::max)( 0, eval_values.initial_material * 2 - p.material[0] - p.material[1] ) / (eval_values.initial_material * 2);
	if( eval_fast > 0 ) {
		eval_fast += v;
	}
	else if( eval_fast < 0 ) {
		eval_fast -= v;
	}

	short mat[2];
	mat[0] = p.material[0] - popcount( p.bitboards[0].b[bb_type::pawns]) * eval_values.material_values[pieces::pawn];
	mat[1] = p.material[1] - popcount( p.bitboards[1].b[bb_type::pawns]) * eval_values.material_values[pieces::pawn];

	short diff = mat[c] - mat[1-c];
	eval_fast += (diff * eval_values.material_imbalance_scale) / 20;

	return eval_fast;
}

short get_material_value( pieces::type pi )
{
	return eval_values.material_values[pi];
}
