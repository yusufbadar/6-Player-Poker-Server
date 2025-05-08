#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

extern int has_acted[MAX_PLAYERS];
extern int last_raiser;

//Feel free to add your own code. I stripped out most of our solution functions but I left some "breadcrumbs" for anyone lost

// for debugging
void print_game_state( game_state_t *game){
    (void) game;
}

void init_deck(card_t deck[DECK_SIZE], int seed){ //DO NOT TOUCH THIS FUNCTION
    srand(seed);
    int i = 0;
    for(int r = 0; r<13; r++){
        for(int s = 0; s<4; s++){
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE; ++i) {
        int j = rand() % DECK_SIZE;
        card_t tmp = deck[i];
        deck[i]   = deck[j];
        deck[j]   = tmp;
    }
}
//You dont need to use this if you dont want, but we did.
void init_game_state(game_state_t *game, int starting_stack, int random_seed)
{
    memset(game, 0, sizeof(game_state_t));
    init_deck(game->deck, random_seed);

    game->next_card = 0;
    game->round_stage = ROUND_JOIN;

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        game->player_stacks[i] = starting_stack;
        game->player_status[i] = PLAYER_LEFT;
        game->sockets[i] = -1;
        for (int c = 0; c < HAND_SIZE; ++c)
            game->player_hands[i][c] = NOCARD;
    }
    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        game->community_cards[i] = NOCARD;

    game->dealer_player = 0;
    game->current_player = 1;
}
static inline player_id_t next_active_player(game_state_t *g, player_id_t start)
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE) return p;
    }
    return -1;
}
void reset_game_state(game_state_t *game)
{
    round_stage_t prev_stage = game->round_stage;

    shuffle_deck(game->deck);
    game->next_card = 0;
    game->round_stage = ROUND_INIT;
    game->pot_size = 0;
    game->highest_bet  = 0;

    if (prev_stage != ROUND_JOIN) {
        game->dealer_player  = (game->dealer_player + 1) % MAX_PLAYERS;
    }
    game->current_player = next_active_player(game, (game->dealer_player + 1) % MAX_PLAYERS);

    for (int i = 0; i < MAX_COMMUNITY_CARDS; ++i)
        game->community_cards[i] = NOCARD;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        game->current_bets[p] = 0;

        if (game->player_status[p] != PLAYER_LEFT)
            game->player_status[p] = PLAYER_ACTIVE;

        for (int c = 0; c < HAND_SIZE; ++c)
            game->player_hands[p][c] = NOCARD;
    }
}

void server_join(game_state_t *game) {
    //This function was called to get the join packets from all players
    (void) game;
}

int server_ready(game_state_t *game) {
    (void)game;
    return 0;
}

void server_deal(game_state_t *g)
{
    int first = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE) {
            first = p;
            break;
        }
    }

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        int seat = (first + i) % MAX_PLAYERS;
        if (g->player_status[seat] == PLAYER_ACTIVE) {
            g->player_hands[seat][0] = g->deck[g->next_card++];
            g->player_hands[seat][1] = g->deck[g->next_card++];
        }
    }

    g->round_stage = ROUND_PREFLOP;
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
    memset(has_acted, 0, sizeof(has_acted));
    last_raiser = -1;
}

int server_bet(game_state_t *g) { 
    return check_betting_end(g); 
}
int check_betting_end(game_state_t *g)
{
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE) {
            if (g->current_bets[p] != g->highest_bet)
                return 0;
            if (!has_acted[p])
                return 0;
        }
    }
    return 1;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage) {

    case ROUND_PREFLOP:             /* -> FLOP */
        g->next_card++;                       // **burn**
        for (int i = 0; i < 3; ++i)
            g->community_cards[i] = g->deck[g->next_card++];
        g->round_stage = ROUND_FLOP;
        break;

    case ROUND_FLOP:                /* -> TURN */
        g->next_card++;                       // **burn**
        g->community_cards[3] = g->deck[g->next_card++];
        g->round_stage = ROUND_TURN;
        break;

    case ROUND_TURN:                /* -> RIVER */
        g->next_card++;                       // **burn**
        g->community_cards[4] = g->deck[g->next_card++];
        g->round_stage = ROUND_RIVER;
        break;

    default:
        break;
    }

    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
    g->current_player =
        next_active_player(g, (g->dealer_player + 1) % MAX_PLAYERS);
}

int evaluate_hand(game_state_t *game, player_id_t pid) {
    //We wrote a function to compare a "value" for each players hand (to make comparison easier)
    //Feel free to not do this.
    (void) game;
    (void) pid;
    return 0;
}

int find_winner(game_state_t *game) {
    //We wrote this function that looks at the game state and returns the player id for the best 5 card hand.
    (void) game;
    return -1;
}