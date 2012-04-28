#include "endgame.hpp"
#include "tables.hpp"

#include <iostream>

enum piece_masks : uint64_t {
	white_pawn =   0x1ull,
	white_knight = 0x10ull,
	white_bishop = 0x100ull,
	white_rook =   0x1000ull,
	white_queen =  0x10000ull,
	black_pawn =   0x100000ull,
	black_knight = 0x1000000ull,
	black_bishop = 0x10000000ull,
	black_rook =   0x100000000ull,
	black_queen =  0x1000000000ull,
	max         =  0x10000000000ull
};


bool evaluate_endgame( position const& p, short& result )
{
	uint64_t piece_sum = 0;

	for( int c = 0; c < 2; ++c ) {
		for( int piece = pieces::pawn; piece < pieces::king; ++piece ) {
			uint64_t count = popcount( p.bitboards[c].b[piece] );
			piece_sum |= count << ((piece - 1 + (c ? 5 : 0)) * 4);
		}
	}

	switch( piece_sum ) {
	// Totally insufficient material.
	case 0:
	case white_knight:
	case black_knight:
	case white_bishop:
	case black_bishop:
		result = result::draw;
		return true;

	// Usually drawn pawnless endgames, equal combinations. Mate only possible if enemy is one epic moron.
	case white_bishop + black_knight:
	case white_knight + black_bishop:
	case white_bishop + black_bishop:
	case white_knight + black_knight:
		result = result::draw;
		return true;

	// Usually drawn pawnless endgames, equal combinations. Still possible to force a mate if enemy loses a piece.
	case white_queen + black_queen:
	case white_rook + black_rook:
		result = (p.base_eval.eg() - p.material[0].eg() + p.material[1].eg()) / 5;
		return true;

	// Usually drawn pawnless endgames, imbalanced combinations.
	case white_rook + black_bishop:
	case white_bishop + black_rook:
	case white_rook + black_knight:
	case white_knight + black_rook:
	case white_rook + white_bishop + black_rook:
	case white_rook + black_rook + black_bishop:
	case white_rook + white_knight + black_rook:
	case white_rook + black_rook + black_knight:
	case white_queen + 2 * black_knight:
	case 2 * white_knight + black_queen:
	case white_queen + white_bishop + black_queen:
	case white_queen + black_bishop + black_queen:
		result = (p.base_eval.eg() - p.material[0].eg() + p.material[1].eg()) / 5;
		return true;
	// Drawn but dangerous for the defender.
	case white_queen + 2 * black_bishop:
	case white_queen + white_knight + black_queen:
		result = p.base_eval.eg() - p.material[0].eg() + p.material[1].eg() + 100;
		return true;
	case black_queen + 2 * white_bishop:
	case white_queen + black_knight + black_queen:
		result = p.base_eval.eg() - p.material[0].eg() + p.material[1].eg() - 100;
		return true;

	// Usually drawn endgames pawn vs. minor piece
	// If not drawn, search will hopefully save us.
	case white_bishop + black_pawn:
	case black_bishop + white_pawn:
	case white_knight + black_pawn:
	case black_knight + white_pawn:
		result = (p.base_eval.eg() - p.material[0].eg() + p.material[1].eg()) / 5;
		return true;

	// Drawn if pawn on a or h file, enemy king in front
	case white_pawn:
		if( p.bitboards[color::white].b[bb_type::pawns] & 0x8181818181818181ull ) {
			uint64_t pawn = bitscan(p.bitboards[color::white].b[bb_type::pawns]);
			if( passed_pawns[color::white][pawn] & p.bitboards[color::black].b[bb_type::king] ) {
				result = result::draw;
				return true;
			}
		}
		break;
	case black_pawn:
		if( p.bitboards[color::black].b[bb_type::pawns] & 0x8181818181818181ull ) {
			uint64_t pawn = bitscan(p.bitboards[color::black].b[bb_type::pawns]);
			if( passed_pawns[color::black][pawn] & p.bitboards[color::white].b[bb_type::king] ) {
				result = result::draw;
				return true;
			}
		}
		break;

	// Drawn if bishop doesn't control the promotion square and enemy king is on promotion square or next to it
	case white_bishop + white_pawn:
		if( p.bitboards[color::white].b[bb_type::pawns] & 0x8181818181818181ull ) {
			uint64_t pawn = bitscan(p.bitboards[color::white].b[bb_type::pawns]);
			uint64_t enemy_king_mask = (pawn % 8) ? 0xc0c0000000000000ull : 0x0303000000000000ull;
			if( enemy_king_mask & p.bitboards[color::black].b[bb_type::king] ) {
				bool promotion_square_is_light = (pawn % 8) == 0;
				bool light_squared_bishop = p.bitboards[color::white].b[bb_type::bishops] & 0x55aa55aa55aa55aaull;
				if( promotion_square_is_light != light_squared_bishop ) {
					result = result::draw;
					return true;
				}
			}
		}
		break;
	case black_bishop + black_pawn:
		if( p.bitboards[color::black].b[bb_type::pawns] & 0x8181818181818181ull ) {
			uint64_t pawn = bitscan(p.bitboards[color::black].b[bb_type::pawns]);
			uint64_t enemy_king_mask = (pawn % 8) ? 0xc0c0ull : 0x0303ull;
			if( enemy_king_mask & p.bitboards[color::white].b[bb_type::king] ) {
				bool promotion_square_is_light = (pawn % 8) == 7;
				bool light_squared_bishop = p.bitboards[color::black].b[bb_type::bishops] & 0x55aa55aa55aa55aaull;
				if( promotion_square_is_light != light_squared_bishop ) {
					result = (p.base_eval.eg() - p.material[0].eg() + p.material[1].eg()) / 5;
					return true;
				}
			}
		}
		break;
	default:
		break;
	}

	return false;
}
