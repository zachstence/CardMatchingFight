#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <pthread.h>
#include <random>
#include <queue>
#include <algorithm>
#include <stdexcept>

using std::cout;
using std::endl;
using std::setw;
using std::stringstream;
using std::string;
using std::stoi;
using std::ofstream;
using std::deque;
using std::random_device;
using std::mt19937;
using std::uniform_int_distribution;
using std::invalid_argument;

// Global constants for game type
const static int NUM_SUITS = 4;
const static int NUM_RANKS = 13;
const static int NUM_PLAYERS = 3;
const static int NUM_ROUNDS = 3;

/**
 * Structure for easy random number generation.
 */
struct RNG {
    random_device r;
    mt19937 mt;
    uniform_int_distribution<int> dist;

    RNG() : mt(r()), dist(0, 1) {}
    explicit RNG(int seed) : mt(seed), dist(0, 1) {}
    int gen() { return dist(mt); }
};

/**
 * Structure used to pass multiple arguments into pthreads.
 */
struct Args {
    RNG* rng;
    int index;
};

/**
 * Creates a deck containing NUM_SUITS copies of cards with NUM_RANKS ranks.
 * @return A deque containg the cards as integers (suit not important).
 */
deque<int> createDeck() {
    deque<int> deck;
    for (int s = 0; s < NUM_SUITS; s++) {
        for (int r = 0; r < NUM_RANKS; r++) {
            deck.push_back(r);
        }
    }
    return deck;
}

/**
 * Shuffles a deck.
 * @param deck The deck to shuffle.
 * @param rng The RNG object to use to shuffle the deck.
 */
void shuffleDeck(deque<int> &deck, RNG *rng) {
    std::shuffle(deck.begin(), deck.end(), rng->mt);
}

/**
 * Converts a deck into a string representation for easy printing to the screen or a log file.
 * @param deck The deck to convert to a string.
 * @return A string representation of the deck with each card separated by a space.
 */
string deckToString(const deque<int> &deck) {
    stringstream ss;
    for (auto x : deck) {
        ss << setw(2) << x << " ";
    }
    return ss.str();
}


// Deck and hands data structures to hold cards during gameplay
auto deck = createDeck();
int hands[NUM_PLAYERS];

// Mutexes to control printing to screen and log file, as well as log file for threads to print to
pthread_mutex_t screenMutex;
ofstream fout;
pthread_mutex_t logFileMutex;

bool gameFinished = false;

// Boolean, mutex, and condition variables controlling whether or not players start a round
bool play = false;
pthread_mutex_t playMutex;
pthread_cond_t playCond;

// Booleans, mutexes, and condition variables that allow players to signal when they are ready to start a round
bool playerReady[NUM_PLAYERS];
pthread_mutex_t playerReadyMutex[NUM_PLAYERS];
pthread_cond_t playerReadyCond[NUM_PLAYERS];

// Booleans, mutexes, and condition variables that allow the dealer and players to signal players when it is their turn
bool turns[NUM_PLAYERS];
pthread_cond_t turnCond[NUM_PLAYERS];
pthread_mutex_t turnMutex[NUM_PLAYERS];

/**
 * Dealer function to be ran in its own thread. Starts rounds and communicates with players to control the flow of the
 * game.
 * @param param Arguments passed to the thread on its creation, expected to be an Args*.
 * @return nullptr.
 */
void *dealer(void *param) {

    // Get RNG passed into thread
    Args* args = (Args *) param;
    RNG* rng = args->rng;
    delete (Args *) param;

    // Play game through NUM_ROUNDS rounds
    for (int round = 0; round < NUM_ROUNDS; round++) {

        // Wait for players to be ready
        for (int i = 0; i < NUM_PLAYERS; i++) {
            pthread_mutex_lock(&playerReadyMutex[i]);
            if (!playerReady[i]) pthread_cond_wait(&playerReadyCond[i], &playerReadyMutex[i]);
            pthread_mutex_unlock(&playerReadyMutex[i]);
        }

        // Once players are ready, set them to not ready
        for (int i = 0; i < NUM_PLAYERS; i++) {
            pthread_mutex_lock(&playerReadyMutex[i]);
            playerReady[i] = false;
            pthread_mutex_unlock(&playerReadyMutex[i]);
        }

        // Shuffle deck
        pthread_mutex_lock(&screenMutex);
        cout << "\n----- ROUND " << round + 1 << " / " << NUM_ROUNDS << " -----" << endl;
        pthread_mutex_unlock(&screenMutex);
        pthread_mutex_lock(&logFileMutex);
        fout << "DEALER  : shuffle (round " << round + 1 << " / " << NUM_ROUNDS << ")" << endl;
        pthread_mutex_unlock(&logFileMutex);
        shuffleDeck(deck, rng);

        // Deal one card to each player
        for (auto &hand : hands) {
            int card = deck.front();
            deck.pop_front();
            hand = card;
        }

        // Reset player turns
        for (auto &turn : turns) {
            turn = false;
        }

        // Signal start play
        pthread_mutex_lock(&playMutex);
        play = true;
        pthread_cond_broadcast(&playCond);
        pthread_mutex_unlock(&playMutex);

        // Signal corresponding player to begin playing
        int startingPlayer = round % NUM_PLAYERS;
        pthread_mutex_lock(&turnMutex[startingPlayer]);
        turns[startingPlayer] = true;
        pthread_cond_signal(&turnCond[startingPlayer]);
        pthread_mutex_unlock(&turnMutex[startingPlayer]);

        // Wait until players are done with round
        pthread_mutex_lock(&playMutex);
        if (play) pthread_cond_wait(&playCond, &playMutex);
        pthread_mutex_unlock(&playMutex);

    }

    // Wait for players to be ready
    for (int i = 0; i < NUM_PLAYERS; i++) {
        pthread_mutex_lock(&playerReadyMutex[i]);
        if (!playerReady[i]) pthread_cond_wait(&playerReadyCond[i], &playerReadyMutex[i]);
        pthread_mutex_unlock(&playerReadyMutex[i]);
    }

    // Wake up players at end of game
    gameFinished = true;
    for (int i = 0; i < NUM_PLAYERS; i++) {
        pthread_mutex_lock(&turnMutex[i]);
        turns[i] = true;
        pthread_cond_signal(&turnCond[i]);
        pthread_mutex_unlock(&turnMutex[i]);
    }

    pthread_mutex_lock(&playMutex);
    play = true;
    pthread_cond_broadcast(&playCond);
    pthread_mutex_unlock(&playMutex);

    return nullptr;

}

/**
 * Player function to be ran in its own thread. Plays the game and communicates with other players to control the flow
 * of the game.
 * @param param Arguments passed to the thread on its creation, expected to be an Args*.
 * @return nullptr.
 */
void *player(void *param) {

    // Get index and RNG passed into thread
    Args* args = (Args *) param;
    int playerIndex = args->index;
    RNG* rng = args->rng;
    delete (Args *) param;

    int nextPlayerIndex = (playerIndex + 1) % NUM_PLAYERS;

    // Signal ready for first round
    pthread_mutex_lock(&playerReadyMutex[playerIndex]);
    playerReady[playerIndex] = true;
    pthread_cond_signal(&playerReadyCond[playerIndex]);
    pthread_mutex_unlock(&playerReadyMutex[playerIndex]);

    // Play game
    while (!gameFinished) {

        // Wait for the player's turn
        pthread_mutex_lock(&turnMutex[playerIndex]);
        if (!turns[playerIndex]) pthread_cond_wait(&turnCond[playerIndex], &turnMutex[playerIndex]);
        pthread_mutex_unlock(&turnMutex[playerIndex]);

        // Wait for dealer to allow play
        pthread_mutex_lock(&playMutex);
        if (!play) {

            pthread_mutex_lock(&logFileMutex);
            fout << "PLAYER " << playerIndex + 1 << ": round completed" << endl;
            pthread_mutex_unlock(&logFileMutex);

            if (hands[playerIndex] != -1) {
                deck.push_back(hands[playerIndex]);
                hands[playerIndex] = -1;
            }

            // Wake up next player to have them discard their hand and wait for new round
            pthread_mutex_lock(&turnMutex[nextPlayerIndex]);
            turns[nextPlayerIndex] = true;
            pthread_cond_signal(&turnCond[nextPlayerIndex]);
            pthread_mutex_unlock(&turnMutex[nextPlayerIndex]);

            // Signal ready for new round
            pthread_mutex_lock(&playerReadyMutex[playerIndex]);
            playerReady[playerIndex] = true;
            pthread_cond_signal(&playerReadyCond[playerIndex]);
            pthread_mutex_unlock(&playerReadyMutex[playerIndex]);

            pthread_cond_wait(&playCond, &playMutex);
            pthread_mutex_unlock(&playMutex);
            continue;
        } else {
            pthread_mutex_unlock(&playMutex);
        }

        if (gameFinished) break;

        fout << "DECK    : " << deckToString(deck) << endl;

        // When its the player's turn, print what card it has
        pthread_mutex_lock(&logFileMutex);
        fout << "PLAYER " << playerIndex + 1 << ": hand " << hands[playerIndex] << endl;
        pthread_mutex_unlock(&logFileMutex);

        // Draw new card
        int card = deck.front();
        deck.pop_front();

        pthread_mutex_lock(&logFileMutex);
        fout << "PLAYER " << playerIndex + 1 << ": draws " << card << endl;
        pthread_mutex_unlock(&logFileMutex);

        // Print game status
        pthread_mutex_lock(&screenMutex);
        cout << endl;
        for (int i = 0; i < NUM_PLAYERS; i++) {
            cout << "PLAYER " << i + 1 << ":" << endl;
            if (i == playerIndex && card == hands[playerIndex]) {
                cout << "  HAND " << hands[i] << " " << card << endl;
                cout << "  WIN yes" << endl;
            } else {
                cout << "  HAND " << hands[i] << endl;
                cout << "  WIN no" << endl;
            }
        }
        cout << "DECK: " << deckToString(deck) << endl;
        pthread_mutex_unlock(&screenMutex);

        // If drawn card matches card in hand, player wins
        if (card == hands[playerIndex]) {

            pthread_mutex_lock(&logFileMutex);
            fout << "PLAYER " << playerIndex + 1 << ": hand " << card << " " << hands[playerIndex] << endl;
            fout << "PLAYER " << playerIndex + 1 << ": wins" << endl;
            pthread_mutex_unlock(&logFileMutex);

            // Discard drawn card
            deck.push_back(card);

            // When a player wins, stop play
            pthread_mutex_lock(&playMutex);
            play = false;
            pthread_cond_signal(&playCond);
            pthread_mutex_unlock(&playMutex);

        }
            // Otherwise, discard a card at random
        else {
            int discard;
            int i = rng->gen();
            if (i == 0) {
                deck.push_back(card);
                discard = card;
            } else {
                discard = hands[playerIndex];
                deck.push_back(discard);
                hands[playerIndex] = card;
            }

            pthread_mutex_lock(&logFileMutex);
            fout << "PLAYER " << playerIndex + 1 << ": discards " << discard << endl;
            pthread_mutex_unlock(&logFileMutex);

            // Signal next player
            pthread_mutex_lock(&turnMutex[playerIndex]);
            pthread_mutex_lock(&turnMutex[nextPlayerIndex]);
            turns[playerIndex] = false;
            turns[nextPlayerIndex] = true;
            pthread_cond_signal(&turnCond[nextPlayerIndex]);
            pthread_mutex_unlock(&turnMutex[playerIndex]);
            pthread_mutex_unlock(&turnMutex[nextPlayerIndex]);
        }

    }

    pthread_mutex_lock(&logFileMutex);
    fout << "PLAYER " << playerIndex + 1 << ": game finished" << endl;
    pthread_mutex_unlock(&logFileMutex);

    pthread_mutex_lock(&turnMutex[nextPlayerIndex]);
    turns[playerIndex] = false;
    turns[nextPlayerIndex] = true;
    pthread_cond_signal(&turnCond[nextPlayerIndex]);
    pthread_mutex_unlock(&turnMutex[nextPlayerIndex]);

    return nullptr;
}

/**
 * The main method. Parses seed from command line, initializes mutex and condition variables, creates 1 dealer thread
 * and NUM_PLAYERS player threads, and then waits for them all to join once they have finished playing the game.
 */
int main(int argc, char* argv[]) {

    // Get seed and create random number generator
    RNG* rng;
    int seed;
    if (argc >= 2) {
        seed = stoi(argv[1]);
        rng = new RNG(seed);
    } else {
        rng = new RNG();
    }

    // Setup log file
    fout.open("log.txt");

    // Create dealer thread and array to hold player threads
    pthread_t playerThreads[NUM_PLAYERS + 1];
    pthread_t dealerThread;

    // Initialize mutexes, conditions, and data structures
    pthread_mutex_init(&logFileMutex, nullptr);
    pthread_mutex_init(&playMutex, nullptr);
    pthread_cond_init(&playCond, nullptr);
    for (int i = 0; i < NUM_PLAYERS; i++) {
        pthread_mutex_init(&playerReadyMutex[i], nullptr);
        pthread_cond_init(&playerReadyCond[i], nullptr);
        hands[i] = -1;
        playerReady[i] = false;
    }

    // Create players and dealer
    for (int i = 0; i < NUM_PLAYERS; i++) {
        Args* args = new Args {rng, i};
        pthread_create(&playerThreads[i], nullptr, player, args);
    }
    Args* args = new Args {rng, -1};
    pthread_create(&dealerThread, nullptr, dealer, args);

    // Wait for players and dealer to finish game
    for (int i = 0; i < NUM_PLAYERS; i++) {
        pthread_join(playerThreads[i], nullptr);
    }
    pthread_join(dealerThread, nullptr);

    return 0;

}