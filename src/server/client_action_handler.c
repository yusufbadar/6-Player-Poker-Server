#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

/*  has_acted is defined in the server translation unit so we only reference it
    here.  Each element is reset to zero at the start of a betting round and set
    to one once a player has taken a valid action in that round. */
extern int has_acted[MAX_PLAYERS];


int handle_client_action(game_state_t *g, player_id_t pid, const client_packet_t *in, server_packet_t *out)
{
    if (pid != g->current_player || g->player_status[pid] != PLAYER_ACTIVE) {
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
            has_acted[pid] = 1;
            break;

        case CALL:
            if (to_call <= 0 || to_call > g->player_stacks[pid]) {
                out->packet_type = NACK;
                return -1;
            }
            g->player_stacks[pid] -= to_call;
            g->current_bets [pid] += to_call;
            g->pot_size           += to_call;
            has_acted[pid]         = 1;
            break;

        case RAISE: {
            int raise_to = in->params[0];
            if (raise_to <= g->highest_bet) {
                out->packet_type = NACK;
                return -1;
            }
            int need = raise_to - g->current_bets[pid];
            if (need > g->player_stacks[pid] || need < 0) {
                out->packet_type = NACK;
                return -1;
            }
            g->player_stacks[pid] -= need;
            g->current_bets [pid] += need;
            g->pot_size           += need;
            g->highest_bet         = raise_to;

            for (int p = 0; p < MAX_PLAYERS; ++p) {
                if (g->player_status[p] == PLAYER_ACTIVE)
                    has_acted[p] = (p == pid);
            }
            break;
        }

        case FOLD:
            g->player_status[pid] = PLAYER_FOLDED;
            has_acted[pid]        = 1;
            break;

        default:
            out->packet_type = NACK;
            return -1;
    }

    int nxt = (pid + 1) % MAX_PLAYERS;
    while (nxt != pid) {
        if (g->player_status[nxt] == PLAYER_ACTIVE)
            break;
        nxt = (nxt + 1) % MAX_PLAYERS;
    }
    g->current_player = nxt;

    out->packet_type = ACK;
    return 0;
}


void build_info_packet(game_state_t *g, player_id_t pid, server_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    out->packet_type = INFO;

    info_packet_t *p = &out->info;

    p->player_cards[0] = g->player_hands[pid][0];
    p->player_cards[1] = g->player_hands[pid][1];

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        p->community_cards[i] = NOCARD;

    if (g->round_stage >= ROUND_FLOP) {
        memcpy(p->community_cards, g->community_cards, 3 * sizeof(card_t));
        if (g->round_stage >= ROUND_TURN) {
            p->community_cards[3] = g->community_cards[3];
            if (g->round_stage >= ROUND_RIVER)
                p->community_cards[4] = g->community_cards[4];
        }
    }

    for (int pl = 0; pl < MAX_PLAYERS; ++pl) {
        p->player_stacks[pl] = g->player_stacks[pl];
        p->player_bets [pl]  = g->current_bets[pl];

        switch (g->player_status[pl]) {
            case PLAYER_ACTIVE:
            case PLAYER_ALLIN:  p->player_status[pl] = 1; break;
            case PLAYER_FOLDED: p->player_status[pl] = 0; break;
            default:            p->player_status[pl] = 2; break;
        }
    }

    p->pot_size    = g->pot_size;
    p->dealer      = g->dealer_player;
    p->player_turn = g->current_player;
    p->bet_size    = g->highest_bet;
}


void build_end_packet(game_state_t *g, player_id_t winner, server_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    out->packet_type = END;

    end_packet_t *e = &out->end;

    for (int pl = 0; pl < MAX_PLAYERS; ++pl) {
        e->player_cards[pl][0] = g->player_hands[pl][0];
        e->player_cards[pl][1] = g->player_hands[pl][1];
    }

    memcpy(e->community_cards, g->community_cards,
           MAX_COMMUNITY_CARDS * sizeof(card_t));
    for (int pl = 0; pl < MAX_PLAYERS; ++pl) {
        e->player_stacks[pl] = g->player_stacks[pl];
        switch (g->player_status[pl]) {
            case PLAYER_ACTIVE:
            case PLAYER_ALLIN:  e->player_status[pl] = 1; break;
            case PLAYER_FOLDED: e->player_status[pl] = 0; break;
            default:            e->player_status[pl] = 2; break;
        }
    }

    e->pot_size = g->pot_size;
    e->dealer   = g->dealer_player;
    e->winner   = winner;
}