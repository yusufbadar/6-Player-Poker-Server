#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

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

void shuffle_deck(card_t deck[DECK_SIZE]){ //DO NOT TOUCH THIS FUNCTION
    for(int i = 0; i<DECK_SIZE; i++){
        int j = rand() % DECK_SIZE;
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

//You dont need to use this if you dont want, but we did.
void init_game_state(game_state_t *game, int starting_stack, int random_seed)
{
    memset(game, 0, sizeof(game_state_t));
    init_deck(game->deck, random_seed);
    shuffle_deck(game->deck);

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

void reset_game_state(game_state_t *game)
{
    round_stage_t prev_stage = game->round_stage;

    shuffle_deck(game->deck);
    game->next_card    = 0;
    game->round_stage  = ROUND_INIT;
    game->pot_size     = 0;
    game->highest_bet  = 0;

    if (prev_stage != ROUND_JOIN) {
        game->dealer_player  = (game->dealer_player + 1) % MAX_PLAYERS;
    }
    game->current_player = (game->dealer_player + 1) % MAX_PLAYERS;

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
    //This function updated the dealer and checked ready/leave status for all players
    (void)game;
    return 0;
}

//This was our dealing function with some of the code removed (I left the dealing so we have the same logic)
void server_deal(game_state_t *g)
{
    for (int seat = 0; seat < MAX_PLAYERS; ++seat) {
        if (g->player_status[seat] == PLAYER_ACTIVE) {
            g->player_hands[seat][0] = g->deck[g->next_card++];
            g->player_hands[seat][1] = g->deck[g->next_card++];
        }
    }

    g->round_stage  = ROUND_PREFLOP;
    g->highest_bet  = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
}

int server_bet(game_state_t *g) { 
    return check_betting_end(g); 
}

// Returns 1 if all bets are the same among active players
int check_betting_end(game_state_t *g)
{
    int ref = -1;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE) {
            if (ref == -1)
                ref = g->current_bets[p];

            if (g->current_bets[p] != ref)
                return 0;
        }
    }
    return 1;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage) {
        case ROUND_PREFLOP:
            for (int i = 0; i < 3; ++i) g->community_cards[i] = g->deck[g->next_card++];
            g->round_stage = ROUND_FLOP; break;
        case ROUND_FLOP:
            g->community_cards[3] = g->deck[g->next_card++];
            g->round_stage = ROUND_TURN; break;
        case ROUND_TURN:
            g->community_cards[4] = g->deck[g->next_card++];
            g->round_stage = ROUND_RIVER; break;
        default: break;
    }

    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
    g->current_player = (g->dealer_player + 1) % MAX_PLAYERS;
}

void server_end(game_state_t *game) {
    //This function sends the end packet
    (void) game;
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