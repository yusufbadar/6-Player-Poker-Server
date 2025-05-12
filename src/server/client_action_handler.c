#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdint.h>

#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

static inline int  send_ack (server_packet_t *p) { p->packet_type = ACK;  return  0; }
static inline int  send_nack(server_packet_t *p) { p->packet_type = NACK; return -1; }

static inline void maybe_allin(game_state_t *g, player_id_t pid)
{
    if (g->player_stacks[pid] <= 0) {
        g->player_stacks[pid] = 0;
        if (g->player_status[pid] == PLAYER_ACTIVE)
            g->player_status[pid] = PLAYER_ALLIN;
    }
}

static inline uint8_t visible_status(uint8_t s)
{
    switch (s) {
        case PLAYER_ACTIVE:
        case PLAYER_ALLIN:  return 1;
        case PLAYER_FOLDED: return 0;
        default:            return 2;
    }
}

int handle_client_action(game_state_t *g, player_id_t pid, const client_packet_t *in, server_packet_t *out)
{
    if (g->current_player != pid) {
        return send_nack(out);
    }
    const int to_call = g->highest_bet - g->current_bets[pid];

    switch (in->packet_type) {
        case CHECK: 
            return (to_call == 0) ? send_ack(out) : send_nack(out);
        case CALL:
            if (to_call <= 0 || to_call > g->player_stacks[pid]){
                return send_nack(out);
            }
            g->player_stacks[pid] -= to_call;
            g->current_bets [pid] += to_call;
            g->pot_size           += to_call;
            maybe_allin(g, pid);
            return send_ack(out);

        case RAISE: {
            const int chips_now = in->params[0];

            if (chips_now <= to_call || chips_now > g->player_stacks[pid]) {
                return send_nack(out);
            }
            g->player_stacks[pid] -= chips_now;
            g->current_bets [pid] += chips_now;
            g->pot_size           += chips_now;

            g->highest_bet = g->current_bets[pid];
            maybe_allin(g, pid);
            return send_ack(out);
        }

        case FOLD:
            g->player_status[pid] = PLAYER_FOLDED;
            return send_ack(out);

        default:
            return send_nack(out);
    }
}

static void card_to_string(card_t c, char *buf)
{
    static const char *R = "23456789TJQKA";
    static const char *S = "cdhs";
    buf[0] = R[c >> SUITE_BITS];
    buf[1] = S[c & ((1 << SUITE_BITS) - 1)];
    buf[2] = '\0';
}

void build_info_packet(game_state_t *g, player_id_t pid, server_packet_t *out)
{
    out->packet_type = INFO;
    info_packet_t *info = &out->info;

    info->player_cards[0] = g->player_hands[pid][0];
    info->player_cards[1] = g->player_hands[pid][1];

    for (int k = 0; k < MAX_COMMUNITY_CARDS; ++k)
        info->community_cards[k] = NOCARD;

    switch (g->round_stage) {
        case ROUND_RIVER:
            info->community_cards[4] = g->community_cards[4];
        case ROUND_TURN:
            info->community_cards[3] = g->community_cards[3];
        case ROUND_FLOP:
            memcpy(info->community_cards, g->community_cards, 3 * sizeof(card_t));
        default:
            break;
    }

    for (int seat = 0; seat < MAX_PLAYERS; ++seat) {
        info->player_stacks [seat] = g->player_stacks [seat];
        info->player_bets   [seat] = g->current_bets  [seat];
        info->player_status [seat] = visible_status(g->player_status[seat]);
    }

    info->pot_size    = g->pot_size;
    info->dealer      = g->dealer_player;
    info->player_turn = g->current_player;
    info->bet_size    = g->highest_bet;
}

void build_end_packet(game_state_t *g, player_id_t winner, server_packet_t *out)
{
    out->packet_type = END;
    end_packet_t *e  = &out->end;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        e->player_cards [p][0] = g->player_hands[p][0];
        e->player_cards [p][1] = g->player_hands[p][1];
        e->player_stacks [p]   = g->player_stacks[p];
        e->player_status[p]    = visible_status(g->player_status[p]);
    }

    memcpy(e->community_cards, g->community_cards, MAX_COMMUNITY_CARDS * sizeof(card_t));

    e->pot_size = g->pot_size;
    e->dealer   = g->dealer_player;
    e->winner   = winner;
}