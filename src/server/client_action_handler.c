#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"
#include "logs.h"

static void maybe_allin(game_state_t *g, player_id_t pid)
{
    if (g->player_stacks[pid] == 0 && g->player_status[pid] == PLAYER_ACTIVE)
        g->player_status[pid] = PLAYER_ALLIN;
}

int handle_client_action(game_state_t *game,
                         player_id_t       pid,
                         const client_packet_t *in,
                         server_packet_t        *out)
{
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
            game->current_bets [pid] += to_call;
            game->pot_size           += to_call;
            maybe_allin(game, pid);
            out->packet_type = ACK;
            return 0;

        case RAISE: {
            int raise_amt = in->params[0];
            if (raise_amt <= game->highest_bet) {
                out->packet_type = NACK;
                return -1;
            }
            int need = raise_amt - game->current_bets[pid];
            if (need <= 0 || need > game->player_stacks[pid]) {
                out->packet_type = NACK;
                return -1;
            }
            game->player_stacks[pid] -= need;
            game->current_bets [pid] += need;
            game->highest_bet         = raise_amt;
            game->pot_size           += need;
            maybe_allin(game, pid);
            out->packet_type = ACK;
            return 0;
        }

        case FOLD:
            game->player_status[pid] = PLAYER_FOLDED;
            out->packet_type         = ACK;
            return 0;

        default:
            out->packet_type = NACK;
            return -1;
    }
}
static void card_to_string(card_t c, char *buf)
{
    static const char *R="23456789TJQKA", *S="cdhs";
    buf[0]=R[c>>SUITE_BITS]; buf[1]=S[c&((1<<SUITE_BITS)-1)]; buf[2]='\0';
}

void build_info_packet(game_state_t *g, player_id_t pid, server_packet_t *out)
{
    out->packet_type = INFO;
    info_packet_t *i = &out->info;

    i->player_cards[0] = g->player_hands[pid][0];
    i->player_cards[1] = g->player_hands[pid][1];
    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c) i->community_cards[c] = NOCARD;
    if (g->round_stage >= ROUND_FLOP) {
        memcpy(i->community_cards, g->community_cards, 3*sizeof(card_t));
        if (g->round_stage >= ROUND_TURN) {
            i->community_cards[3] = g->community_cards[3];
            if (g->round_stage >= ROUND_RIVER)
                i->community_cards[4] = g->community_cards[4];
        }
    }

    for (int p=0;p<MAX_PLAYERS;++p){
        i->player_stacks[p]=g->player_stacks[p];
        i->player_bets  [p]=g->current_bets [p];
        if (g->player_status[p]==PLAYER_ACTIVE||g->player_status[p]==PLAYER_ALLIN)
            i->player_status[p]=1;
        else if (g->player_status[p]==PLAYER_FOLDED)
            i->player_status[p]=0;
        else
            i->player_status[p]=2;
    }

    i->pot_size    = g->pot_size;
    i->dealer      = g->dealer_player;
    i->player_turn = g->current_player;
    i->bet_size    = g->highest_bet;

    log_info("[INFO_PACKET] pot_size=%d, player_turn=%d, dealer=%d, bet_size=%d",
             i->pot_size, i->player_turn, i->dealer, i->bet_size);

    char b0[4], b1[4]; card_to_string(i->player_cards[0],b0);
    card_to_string(i->player_cards[1],b1);
    log_info("[INFO_PACKET] Your Cards: %s %s", b0, b1);

    for (int c=0;c<MAX_COMMUNITY_CARDS;++c)
        if (i->community_cards[c]!=NOCARD){
            char bc[4]; card_to_string(i->community_cards[c],bc);
            log_info("[INFO_PACKET] Community Card %d: %s", c, bc);
        }

    for (int p=0;p<MAX_PLAYERS;++p)
        log_info("[INFO_PACKET] Player %d: stack=%d, bet=%d, status=%d",
                 p, i->player_stacks[p], i->player_bets[p], i->player_status[p]);
}
void build_end_packet(game_state_t *g, player_id_t winner, server_packet_t *out)
{
    out->packet_type = END;
    end_packet_t *e = &out->end;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        e->player_cards[p][0] = g->player_hands[p][0];
        e->player_cards[p][1] = g->player_hands[p][1];
        e->player_stacks[p]   = g->player_stacks[p];

        if (g->player_status[p] == PLAYER_ACTIVE ||
            g->player_status[p] == PLAYER_ALLIN)
            e->player_status[p] = 1;
        else if (g->player_status[p] == PLAYER_FOLDED)
            e->player_status[p] = 0;
        else
            e->player_status[p] = 2;
    }

    for (int c = 0; c < MAX_COMMUNITY_CARDS; ++c)
        e->community_cards[c] = g->community_cards[c];

    e->pot_size = g->pot_size;
    e->dealer   = g->dealer_player;
    e->winner   = winner;
}