#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

int handle_client_action(game_state_t *game, player_id_t pid, const client_packet_t *in, server_packet_t *out) {
    if (game->current_player != pid) {
        out->packet_type = NACK;
        return -1;
    }

    int to_call = game->highest_bet - game->current_bets[pid];

    switch (in->packet_type) {
        case CHECK:
            if (to_call != 0) {
                out->packet_type = NACK;
                return -1;
            }
            out->packet_type = ACK;
            return 0;

        case CALL:
            if (to_call <= 0 || to_call > game->player_stacks[pid]) {
                out->packet_type = NACK;
                return -1;
            }
            game->player_stacks[pid] -= to_call;
            game->current_bets[pid] += to_call;
            game->pot_size += to_call;
            out->packet_type = ACK;
            return 0;

        case RAISE: {
            int raise_amt = in->params[0];
            if (raise_amt <= game->highest_bet) {
                out->packet_type = NACK;
                return -1;
            }
            int need = raise_amt - game->current_bets[pid];
            if (need > game->player_stacks[pid] || need < 0) {
                out->packet_type = NACK;
                return -1;
            }
            game->player_stacks[pid] -= need;
            game->current_bets[pid] += need;
            game->highest_bet = raise_amt;
            game->pot_size += need;
            out->packet_type = ACK;
            return 0;
        }

        case FOLD:
            game->player_status[pid] = PLAYER_FOLDED;
            out->packet_type = ACK;
            return 0;

        default:
            out->packet_type = NACK;
            return -1;
    }
}

void build_info_packet(game_state_t *g, player_id_t pid, server_packet_t *out) {
    out->packet_type = INFO;
    info_packet_t *i = &out->info;

    i->player_cards[0] = g->player_hands[pid][0];
    i->player_cards[1] = g->player_hands[pid][1];

    for (int x = 0; x < MAX_COMMUNITY_CARDS; ++x) {
        i->community_cards[x] = NOCARD;
    }

    if (g->round_stage >= ROUND_FLOP) {
        memcpy(i->community_cards, g->community_cards, 3 * sizeof(card_t));
        if (g->round_stage >= ROUND_TURN) {
            i->community_cards[3] = g->community_cards[3];
            if (g->round_stage >= ROUND_RIVER) {
                i->community_cards[4] = g->community_cards[4];
            }
        }
    }

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        i->player_stacks[p] = g->player_stacks[p];
        i->player_bets[p] = g->current_bets[p];
        if (g->player_status[p] == PLAYER_ACTIVE || g->player_status[p] == PLAYER_ALLIN)
            i->player_status[p] = 1;
        else if (g->player_status[p] == PLAYER_FOLDED)
            i->player_status[p] = 0;
        else
            i->player_status[p] = 2;
    }

    i->pot_size = g->pot_size;
    i->dealer = g->dealer_player;
    i->player_turn = g->current_player;
    i->bet_size = g->highest_bet;
}

void build_end_packet(game_state_t *g, player_id_t winner, server_packet_t *out) {
    out->packet_type = END;
    end_packet_t *e = &out->end;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        e->player_cards[p][0] = g->player_hands[p][0];
        e->player_cards[p][1] = g->player_hands[p][1];
        e->player_stacks[p] = g->player_stacks[p];

        if (g->player_status[p] == PLAYER_ACTIVE || g->player_status[p] == PLAYER_ALLIN)
            e->player_status[p] = 1;
        else if (g->player_status[p] == PLAYER_FOLDED)
            e->player_status[p] = 0;
        else
            e->player_status[p] = 2;
    }

    for (int x = 0; x < MAX_COMMUNITY_CARDS; ++x)
        e->community_cards[x] = g->community_cards[x];

    e->pot_size = g->pot_size;
    e->dealer = g->dealer_player;
    e->winner = winner;
}
