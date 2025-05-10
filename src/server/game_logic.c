#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define RANK_MASK  0xF0
#define SUIT_MASK  0x0F

extern int has_acted[MAX_PLAYERS];
extern int last_raiser;

// for debugging
void print_game_state(game_state_t *game){
    (void) game;
}

void init_deck(card_t deck[DECK_SIZE], int seed){ //DO NOT TOUCH THIS FUNCTION
    srand(seed);
    int i = 0;
    for (int s = 0; s < 4;  ++s)
        for (int r = 0; r < 13; ++r)
            deck[i++] = (r << SUITE_BITS) | s;
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE - 1; ++i) {
        int j = i + rand() % (DECK_SIZE - i);
        card_t tmp = deck[i];
        deck[i]    = deck[j];
        deck[j]    = tmp;
    }
}

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

player_id_t next_active_player(game_state_t *g, player_id_t start)
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE) return p;
    }
    return -1;
}

void reset_game_state(game_state_t *g)
{
    shuffle_deck(g->deck);
    g->next_card   = 0;

    if (g->round_stage != ROUND_JOIN) {
        int prev = g->dealer_player;
        for (int i = 1; i <= MAX_PLAYERS; ++i) {
            int cand = (prev + i) % MAX_PLAYERS;
            if (g->player_status[cand] != PLAYER_LEFT) {
                g->dealer_player = cand;
                break;
            }
        }
    }

    g->round_stage = ROUND_INIT;
    g->pot_size = g->highest_bet = 0;
    
    // Reset all cards
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        g->player_hands[p][0] = NOCARD;
        g->player_hands[p][1] = NOCARD;
    }
    
    memset(g->community_cards, NOCARD, sizeof(g->community_cards));
    memset(g->current_bets, 0, sizeof(g->current_bets));
    memset(has_acted, 0, sizeof(int) * MAX_PLAYERS);
    last_raiser = -1;

    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] != PLAYER_LEFT)
            g->player_status[p] = PLAYER_ACTIVE;
    }

    g->current_player = next_active_player(g, (g->dealer_player + 1) % MAX_PLAYERS);
}

void server_join(game_state_t *game) {
    (void) game;
}

int server_ready(game_state_t *game) {
    (void)game;
    return 0;
}

static player_id_t first_active_after(game_state_t *g, player_id_t start)
{
    for (int i = 1; i <= MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE)
            return p;
    }
    return (player_id_t)-1;
}

static player_id_t first_active_from(game_state_t *g, player_id_t start)
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        player_id_t p = (start + i) % MAX_PLAYERS;
        if (g->player_status[p] == PLAYER_ACTIVE)
            return p;
    }
    return (player_id_t)-1;
}
void server_deal(game_state_t *g)
{
    for (player_id_t pid = 0; pid < MAX_PLAYERS; ++pid) {
        if (g->player_status[pid] == PLAYER_ACTIVE)
            g->player_hands[pid][0] = g->deck[g->next_card++];
    }

    for (player_id_t pid = 0; pid < MAX_PLAYERS; ++pid) {
        if (g->player_status[pid] == PLAYER_ACTIVE)
            g->player_hands[pid][1] = g->deck[g->next_card++];
    }


    g->round_stage = ROUND_PREFLOP;
    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
    memset(has_acted, 0, sizeof(int) * MAX_PLAYERS);
    last_raiser = -1;

    g->current_player = first_active_after(g, g->dealer_player);
    if (g->current_player == -1) {
        g->current_player = first_active_from(g, 0);
    }
}

int server_bet(game_state_t *g) { 
    return check_betting_end(g); 
}

int check_betting_end(game_state_t *g) {
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        if (g->player_status[p] == PLAYER_ACTIVE) {
            if (!has_acted[p] || 
                (g->current_bets[p] < g->highest_bet && 
                 g->player_stacks[p] > 0)) {
                return 0;
            }
        }
    }
    return 1;
}

void server_community(game_state_t *g)
{
    switch (g->round_stage) {    
        case ROUND_PREFLOP:
            for (int i = 0; i < 3; i++) {
                g->community_cards[i] = g->deck[g->next_card++]; 
            }
            g->round_stage = ROUND_FLOP;
            break;
            
        case ROUND_FLOP:
            g->community_cards[3] = g->deck[g->next_card++];
            g->round_stage = ROUND_TURN;
            break;
            
        case ROUND_TURN:
            g->community_cards[4] = g->deck[g->next_card++];
            g->round_stage = ROUND_RIVER;
            break;

        default:
            break;
    }

    g->highest_bet = 0;
    memset(g->current_bets, 0, sizeof(g->current_bets));
    memset(has_acted, 0, sizeof(int) * MAX_PLAYERS);
    last_raiser = -1;
    
    player_id_t start = (g->dealer_player + 1) % MAX_PLAYERS;
    g->current_player = next_active_player(g, start);
}

int get_hand_rank(card_t cards[], int num_cards) {
    enum {
        HIGH_CARD,
        ONE_PAIR,
        TWO_PAIR,
        THREE_OF_A_KIND,
        STRAIGHT,
        FLUSH,
        FULL_HOUSE,
        FOUR_OF_A_KIND,
        STRAIGHT_FLUSH
    };

    int ranks[13] = {0};
    int suits[4] = {0};
    
    for (int i = 0; i < num_cards; i++) {
        int rank = ((cards[i] & RANK_MASK) >> 4) + 2;
        int suit = cards[i] & SUIT_MASK;
        ranks[rank-2]++;  // Index 0 is rank 2
        suits[suit]++;
    }
    
    int flush_suit = -1;
    for (int i = 0; i < 4; i++) {
        if (suits[i] >= 5) flush_suit = i;
    }
    
    int straight_high = -1;
    int consecutive = 0;
    for (int i = 0; i < 13; i++) {
        if (ranks[i] > 0) {
            consecutive++;
            if (consecutive >= 5) straight_high = i + 2;
        } else {
            consecutive = 0;
        }
    }
    
    if (ranks[0] > 0 && ranks[1] > 0 && ranks[2] > 0 && ranks[3] > 0 && ranks[12] > 0) {
        straight_high = 5; 
    }
    
    int num_pairs = 0;
    int pair_rank = -1;
    int trips_rank = -1;
    int quads_rank = -1;
    
    for (int i = 12; i >= 0; i--) {
        if (ranks[i] == 4) quads_rank = i + 2;
        else if (ranks[i] == 3) trips_rank = i + 2;
        else if (ranks[i] == 2) {
            num_pairs++;
            if (pair_rank == -1) pair_rank = i + 2;
        }
    }
    
    if (straight_high > 0 && flush_suit >= 0) {
        return STRAIGHT_FLUSH * 100 + straight_high;
    }
    
    if (quads_rank > 0) {
        return FOUR_OF_A_KIND * 100 + quads_rank;
    }
    
    if (trips_rank > 0 && num_pairs > 0) {
        return FULL_HOUSE * 100 + trips_rank;
    }
    
    if (flush_suit >= 0) {
        int high_card = 0;
        for (int i = 12; i >= 0; i--) {
            if (ranks[i] > 0) {
                high_card = i + 2;
                break;
            }
        }
        return FLUSH * 100 + high_card;
    }
    
    if (straight_high > 0) {
        return STRAIGHT * 100 + straight_high;
    }
    
    if (trips_rank > 0) {
        return THREE_OF_A_KIND * 100 + trips_rank;
    }
    
    if (num_pairs >= 2) {
        int second_pair = -1;
        int count = 0;
        for (int i = 12; i >= 0; i--) {
            if (ranks[i] == 2) {
                count++;
                if (count == 2) {
                    second_pair = i + 2;
                    break;
                }
            }
        }
        return TWO_PAIR * 100 + pair_rank * 10 + second_pair;
    }
    
    if (pair_rank > 0) {
        return ONE_PAIR * 100 + pair_rank;
    }
    
    int high_card = 0;
    for (int i = 12; i >= 0; i--) {
        if (ranks[i] > 0) {
            high_card = i + 2;
            break;
        }
    }
    return HIGH_CARD * 100 + high_card;
}

int evaluate_hand(game_state_t *game, player_id_t pid) {
    card_t all_cards[7];
    int num_cards = 0;
    
    if (game->player_hands[pid][0] != NOCARD) {
        all_cards[num_cards++] = game->player_hands[pid][0];
    }
    if (game->player_hands[pid][1] != NOCARD) {
        all_cards[num_cards++] = game->player_hands[pid][1];
    }
    
    for (int i = 0; i < MAX_COMMUNITY_CARDS; i++) {
        if (game->community_cards[i] != NOCARD) {
            all_cards[num_cards++] = game->community_cards[i];
        }
    }
    
    return get_hand_rank(all_cards, num_cards);
}

int find_winner(game_state_t *game) {
    int best_score = -1;
    int winner = -1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->player_status[i] == PLAYER_ACTIVE || 
            game->player_status[i] == PLAYER_ALLIN) {
            int score = evaluate_hand(game, i);
            if (score > best_score) {
                best_score = score;
                winner = i;
            }
        }
    }
    return winner;
}