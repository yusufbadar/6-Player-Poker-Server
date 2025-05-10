#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client_action_handler.h"
#include "game_logic.h"

extern int has_acted[MAX_PLAYERS];

/**
 * @brief Processes packet from client and generates a server response packet.
 * 
 * If the action is valid, a SERVER_INFO packet will be generated containing the updated game state.
 * If the action is invalid or out of turn, a SERVER_NACK packet is returned with an optional error message.
 * 
 * @param pid The ID of the client/player who sent the packet.
 * @param in Pointer to the client_packet_t received from the client.
 * @param out Pointer to a server_packet_t that will be filled with the response.
 * @return 0 if successful processing, -1 on NACK or error.
 */
static int to_packet_status(player_status_t s)
{
    switch (s) {
        case PLAYER_FOLDED: return 0;
        case PLAYER_ACTIVE:
        case PLAYER_ALLIN: return 1;
        case PLAYER_LEFT: return 2;
        default: return 2;
    }
}

int handle_client_action(game_state_t *g, player_id_t pid,
    const client_packet_t *in, server_packet_t *out)
{
if (pid != g->current_player || g->player_status[pid] != PLAYER_ACTIVE) {
out->packet_type = NACK; return -1;
}

int cost;

switch (in->packet_type) {

case CHECK:
    if (g->highest_bet != g->current_bets[pid]) {
        out->packet_type = NACK;
        return -1;
    }
    has_acted[pid] = 1;  
    break;

case CALL:
    if (g->highest_bet == 0) {
        has_acted[pid] = 1;
        break;
    }

    if (g->current_bets[pid] == g->highest_bet) {
        has_acted[pid] = 1;
        break;
    }

    cost = g->highest_bet - g->current_bets[pid];
    if (g->player_stacks[pid] < cost) {
        out->packet_type = NACK;
        return -1;
    }
    g->player_stacks[pid] -= cost;
    g->current_bets[pid] += cost;
    g->pot_size          += cost;
    has_acted[pid] = 1;
    break;

case RAISE: {
    int raise_to = in->params[0];    
    if (raise_to <= g->highest_bet) {
        out->packet_type = NACK;
        return -1;
    }
    cost = raise_to - g->current_bets[pid];
    
    if (g->player_stacks[pid] < cost) {
        out->packet_type = NACK;
        return -1;
    }
    g->player_stacks[pid] -= cost;
    g->current_bets[pid] += cost;
    g->pot_size += cost;
    g->highest_bet = raise_to;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == pid)   has_acted[i] = 1;
        else if (g->player_status[i] == PLAYER_ACTIVE)
            has_acted[i] = 0;
    }
     break;
}

case FOLD:
    g->pot_size        += g->current_bets[pid];
    g->current_bets[pid]  = 0;

    g->player_status[pid] = PLAYER_FOLDED;
    has_acted[pid]        = 1;
    g->highest_bet = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g->player_status[i] == PLAYER_ACTIVE &&
            g->current_bets[i]  > g->highest_bet)
            g->highest_bet = g->current_bets[i];
     break;
default:
    out->packet_type = NACK;
    return -1;
}

int next = (pid + 1) % MAX_PLAYERS;
while (next != pid) {
if (g->player_status[next] == PLAYER_ACTIVE) break;
next = (next + 1) % MAX_PLAYERS;
}
g->current_player = next;

out->packet_type = ACK;
return 0;
}

void build_info_packet(game_state_t *game, player_id_t pid, server_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    out->packet_type = INFO;
    info_packet_t *p = &out->info;

    p->player_cards[0] = game->player_hands[pid][0];
    p->player_cards[1] = game->player_hands[pid][1];

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i) {
        p->community_cards[i] = game->community_cards[i];
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        p->player_stacks[i] = game->player_stacks[i];
        p->player_bets[i]   = game->current_bets[i];
        /* instructor‐provided mapping */
        if (game->player_status[i] == PLAYER_ACTIVE ||
            game->player_status[i] == PLAYER_ALLIN) {
            p->player_status[i] = 1;
        } else if (game->player_status[i] == PLAYER_FOLDED) {
            p->player_status[i] = 0;
        } else {
            p->player_status[i] = 2;
        }
    }
    p->pot_size = game->pot_size;
    p->dealer = game->dealer_player;
    p->player_turn = game->current_player;
    p->bet_size = game->highest_bet;
}


void build_end_packet(game_state_t *game, player_id_t winner, server_packet_t *out)
{
    memset(out, 0, sizeof(*out));
    out->packet_type = END;
    end_packet_t *p = &out->end;

    for (int pl = 0; pl < MAX_PLAYERS; ++pl) {
        p->player_cards[pl][0] = game->player_hands[pl][0];
        p->player_cards[pl][1] = game->player_hands[pl][1];
    }

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        p->community_cards[i] = game->community_cards[i];

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        p->player_stacks[i] = game->player_stacks[i];
        if (game->player_status[i] == PLAYER_ACTIVE ||
            game->player_status[i] == PLAYER_ALLIN) {
            p->player_status[i] = 1;
        } else if (game->player_status[i] == PLAYER_FOLDED) {
            p->player_status[i] = 0;
        } else {
            p->player_status[i] = 2;
        }
    }
    p->pot_size = game->pot_size;
    p->dealer = game->dealer_player;
    p->winner = winner;
}
