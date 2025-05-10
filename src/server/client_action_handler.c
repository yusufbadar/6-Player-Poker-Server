#include "client_action_handler.h"
#include <string.h>

int handle_client_action(game_state_t *g, player_id_t pid, 
        const client_packet_t *in, server_packet_t *out) {
    // Verify it's the player's turn
    if (g->current_player != pid || g->player_status[pid] != PLAYER_ACTIVE) {
        out->packet_type = NACK;
        return -1;
    }

    int to_call = g->highest_bet - g->current_bets[pid];

    switch (in->packet_type) {
        case CHECK:
            if (to_call != 0) {
                out->packet_type = NACK;
                return -1;
            }
            break;
            
        case CALL:
            if (to_call <= 0 || to_call > g->player_stacks[pid]) {
                out->packet_type = NACK;
                return -1;
            }
            g->player_stacks[pid] -= to_call;
            g->current_bets[pid] += to_call;
            g->pot_size += to_call;
            break;
            
        case RAISE: {
            int raise_amt = in->params[0];
            if (raise_amt <= g->highest_bet) {
                out->packet_type = NACK;
                return -1;
            }
            int need = raise_amt - g->current_bets[pid];
            if (need > g->player_stacks[pid]) {
                out->packet_type = NACK;
                return -1;
            }
            g->player_stacks[pid] -= need;
            g->current_bets[pid] += need;
            g->highest_bet = raise_amt;
            g->pot_size += need;
            break;
        }
            
        case FOLD:
            g->player_status[pid] = PLAYER_FOLDED;
            break;
            
        default:
            out->packet_type = NACK;
            return -1;
    }

    out->packet_type = ACK;
    return 0;
}

void build_info_packet(game_state_t *g, player_id_t pid, server_packet_t *out) {
    memset(out, 0, sizeof(*out));
    out->packet_type = INFO;
    
    out->info.player_cards[0] = g->player_hands[pid][0];
    out->info.player_cards[1] = g->player_hands[pid][1];
    
    for (int i = 0; i < MAX_COMMUNITY_CARDS; i++) {
        out->info.community_cards[i] = NOCARD;
    }
    
    if (g->round_stage >= ROUND_FLOP) {
        memcpy(out->info.community_cards, g->community_cards, 3 * sizeof(card_t));
        if (g->round_stage >= ROUND_TURN) {
            out->info.community_cards[3] = g->community_cards[3];
            if (g->round_stage >= ROUND_RIVER) {
                out->info.community_cards[4] = g->community_cards[4];
            }
        }
    }
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->info.player_stacks[i] = g->player_stacks[i];
        out->info.player_bets[i] = g->current_bets[i];
        
        if (g->player_status[i] == PLAYER_ACTIVE || g->player_status[i] == PLAYER_ALLIN) {
            out->info.player_status[i] = 1;
        } else if (g->player_status[i] == PLAYER_FOLDED) {
            out->info.player_status[i] = 0;
        } else {
            out->info.player_status[i] = 2;
        }
    }
    
    out->info.pot_size = g->pot_size;
    out->info.dealer = g->dealer_player;
    out->info.player_turn = g->current_player;
    out->info.bet_size = g->highest_bet;
}

void build_end_packet(game_state_t *g, player_id_t winner, server_packet_t *out) {
    memset(out, 0, sizeof(*out));
    out->packet_type = END;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        out->end.player_cards[i][0] = g->player_hands[i][0];
        out->end.player_cards[i][1] = g->player_hands[i][1];
        out->end.player_stacks[i] = g->player_stacks[i];
        
        if (g->player_status[i] == PLAYER_ACTIVE || g->player_status[i] == PLAYER_ALLIN) {
            out->end.player_status[i] = 1;
        } else if (g->player_status[i] == PLAYER_FOLDED) {
            out->end.player_status[i] = 0;
        } else {
            out->end.player_status[i] = 2;
        }
    }
    
    memcpy(out->end.community_cards, g->community_cards, 
        MAX_COMMUNITY_CARDS * sizeof(card_t));
    
    out->end.pot_size = g->pot_size;
    out->end.dealer = g->dealer_player;
    out->end.winner = winner;
}