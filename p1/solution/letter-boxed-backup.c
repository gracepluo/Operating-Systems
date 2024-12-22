#define _GNU_SOURCE
#define _POSIX_C_SOURCE 20089L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define boardSize 26
#define sideSize 26
#define MAX_WORD_LENGTH 100

typedef struct {
    int board[boardSize];
    char* sides[26];
	int rowNum;
} Board;

typedef struct node {
    char* word;
    struct node* next;
} dictReader;

int read_board(Board* board, const char* filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error opening board file: %s\n", filename);
        return 1;
    }
	//char **brd;
	//int num_lines = 0;
    int row = 0;
    size_t length = 0;
    char *line = NULL;

    line = (char *)malloc(sideSize* sizeof(char));

    if( line == NULL)
    {
        fprintf(stderr, "Unable to allocate buffer");
        exit(1);
    }

    
    while (getline(&line, &length, fp) != -1) {
        line[strcspn(line, "\n")] = '\0';  // Remove newline character
        board->sides[row] = strdup(line);
        if( board->sides[row]== NULL)
		{
			fprintf(stderr, "Unable to allocate buffer for the side. \n");
			exit(1);
		}
		//printf("Read side %d: %s\n", row + 1, board->sides[row]);  // Debug print of each side
        row++;
        if (row > boardSize) {
            fprintf(stderr, "Invalid board\n");
			break;
		}
	}
	// brd = malloc(sizeof(char *) * row);
    
	// for (int i = 0; i < row; i++) {
	//    getline(&brd[i], &length, fp);
	// }	   

	
    // while (getline(&line, &length, fp) != -1) {
	// 	board->words[row] = strdup(line);
	// 	row++;
	// }
	free(line);
	fclose(fp);

    board->rowNum = row;
	if (row < 3 || row > boardSize) {
        fprintf(stderr, "Invalid board\n");
        fprintf(stderr, "Too few or too many sides\n");
        return 1;
    }
	int tempChar[boardSize];

    for(int j = 0; j < boardSize; j++){
        tempChar[j] = 0;    
	}


	// for(row = 0; row < board->rowNum; row++){
    //     //tempChar[row] = 0;
	// 	for(int i = 0; i <= board->board[row]; i++){			
	// 		char current_character = board->board[i];
	// 		tempChar[current_character-'a']++; 
	// 		if(tempChar[current_character-'a'] > 1){
	// 			printf("Invalid board\n");
	// 			return 1;
	// 		}
	// 	}
	// }

    for (int i = 0; i < board->rowNum; i++) {
        int rowSize = strlen(board->sides[i]);
        for (int j = 0; j < rowSize; j++) {
            char letter = board->sides[i][j];
            //int idx = tolower(letter) - 'a';
            int idx = letter - 'a';
            if(idx<0 || idx>26){
                printf("Invalid board: non-alphabetic character detected\n");
                return 1;
            }
            tempChar[idx]++;
            if (tempChar[idx] > 1) {
                printf("Invalid board: letter '%c' appears more than once\n", letter);
                return 1;
            }
        }
    }

	
    // fclose(fp);
    //printf("success load board");
    
	printf("Board read successfully:\n");
    for (int i = 0; i < board->rowNum; i++) {
        printf("Side %d: %s\n", i + 1, board->sides[i]);
    }
	
	return row;
}

int read_dict(dictReader *head, char* dictname){
	FILE *dict = fopen(dictname, "r");
    if (!dict) {
        fprintf(stderr, "Error opening dict file: %s\n", dictname);
        return 1;
    }
	int dict_size = 0;
	// head = calloc(1, sizeof(dictReader));
	// head->word = NULL;
    size_t len = 0;
	// head->next = NULL;
	dictReader *curr = head;
    while (getline(&curr->word, &len, dict) != -1) {
		(curr->word)[strcspn(curr->word, "\n")] = '\0';
		curr->next = calloc(1, sizeof(dictReader));
		curr = curr->next;
        dict_size++;
	}
    return dict_size;
}

int is_word_in_dict(dictReader *dict, const char *word) {
    dictReader *curr = dict;
	int wordLen = strlen(word);
    printf ("value of word is %s. %d \n", word, wordLen);
	while (curr) {
			wordLen = strlen(curr->word);
		//printf ("value of curr is %s. %d \n", curr->word, wordLen);
        if (strcmp(curr->word, word) == 0) {
            printf("word is found. \n");
			return 1;  // Word found
        }
        curr = curr->next;
    }
    return 0;  // Word not found
}


int check_solution(Board *board, dictReader *dict, const char *solution) {
    int letter_counts[26] = {0};  // Count of letters on the board
	int letter_used[26] = {0};  // Array to track used letters
    printf("in func check sol");
    // Count letters on the board
    for (int i = 0; i < board->rowNum; i++) {
        int temp = strlen(board->sides[i]);
        for (int j = 0; j < temp; j++) {
            int idx = board->sides[i][j] - 'a';
            if(idx<0 || idx>26){
                printf("Invalid letter\n");
                return 1;
            }
            letter_counts[idx]++;
        }
    }
    // int idx = letter - 'a';
    //     if(idx<0 || idx>26){
    //         printf("Invalid board: non-alphabetic character detected\n");
    //         return 1;
    //     }
    char prev_word[MAX_WORD_LENGTH] = "";  // Store previous word
    //char curr_word[MAX_WORD_LENGTH] = "";  // Store current word
    int solution_pos = 0;
    int solution_len = strlen(solution);
	char* curr_word= strtok(solution, " \t\n");
    char prelast = '\0';
    //for (int i = 0; i < solution_len; i++) {
	    
    // Process each word
    while (curr_word!= NULL) {
        printf("Processing word: %s\n", curr_word);  // Print each word (or process as needed)
        for (int i = 0; curr_word[i] != '\0'; i++) {
            char letter = curr_word[i];
            int letter_idx = letter - 'a';

            // 1. Check if the letter exists on the board
            if (letter_counts[letter_idx] == 0) {
                printf("Used a letter not present on the board\n");
                return 0;
            }
            letter_used[letter_idx]++;  // Track letters used in the solution

            // Accumulate the current word
            //curr_word[solution_pos++] = letter;

            // Check if the word has ended (either a space or the end of the string)
            //if (solution[i + 1] == ' ' || solution[i + 1] == '\0') {
            //    curr_word[solution_pos] = '\0';  // Terminate the current word
                printf("current word is %s . \n", curr_word);
                // 2. Check if the first letter of the current word matches the last letter of the previous word
                if (prelast != '\0' && curr_word[0] != prelast) {
                    printf("First letter of word does not match last letter of previous word\n");
                    return 0;
                }

                // // 3. Check if letters on the same side of the board are used consecutively
                // for (int j = 0; j < board->rowNum; j++) {
                //     if (strchr(board->sides[j], prev_word[strlen(prev_word) - 1]) &&
                //         strchr(board->sides[j], curr_word[0])) {
                //         printf("Same-side letter used consecutively\n");
                //         return 0;
                //     }
                // }

            //}
            // 4. Check if the word is in the dictionary
            if (is_word_in_dict(dict, curr_word) < 1) {
                printf("Word not found in dictionary\n");
                return 0;
            }
            if (curr_word != NULL && strlen(curr_word) > 1) {
                prelast = curr_word[strlen(curr_word) - 1];
                printf("last letter %c\n", prelast);
            }
            

            // Update the previous word and reset the current word position
            //strcpy(prev_word, curr_word);
            //solution_pos = 0;
        
         // Get the next word
        curr_word = strtok(NULL, " \t\n");  // Continue splitting by any whitespace
     }
	
	}

    // 5. Check if all letters on the board were used
    printf("step 5");
    for (int i = 0; i < 26; i++) {
        if (letter_counts[i] > letter_used[i]) {
            printf("Not all letters used\n");
            return 0xDEFF;
        }
    }

    printf("Correct\n");
    return 0xDEADBEEF;
}




int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <board_file> <dictionary_file>\n", argv[0]);
        return 1;
    }
    

    // read_board(, argv[1]);
    dictReader * header = malloc(sizeof(dictReader));
    if(header == NULL){
        return 1;
    }
    // read_dict(header, argv[2]);
    int dictSize = read_dict(header, argv[2]);
    //printf("%i", dictSize);
    //dictReader * curr = header;

    Board * boardRead = malloc(sizeof(Board));
    if(boardRead == NULL){
        return 1;
    }
    int sideNum = read_board(boardRead, argv[1]);
    if (sideNum < 3){
		printf("Invalid board\n");
		return 1;
	}
    //printf("%i", sideNum);
    // for (int i = 0; i < boardRead->rowNum; i++) {
    //     int leng = strlen(boardRead->sides[i]);
    //     for (int j = 0; j < leng; j++) {
    //         //printf("%s", curr->word);
    //         curr = curr->next;
    //     }
    // }


    // for (int i = 0; i < dictSize; i++){
    //     printf("%s", curr->word);
    //     curr = curr->next;
    // }
    //tester that proves board reader works


    // for (int i = 0; i < dictSize; i++){
    //     printf("%s", curr->word);
    //     curr = curr->next;
    // }
    //tester that proves dict reader works
     
    char solution[boardSize * sideSize];  // Buffer for accumulated solution
    //int letter_used[26] = {0};  // Array to track used letters
	char input_line[sideSize] = {'\0'};
	// Initialize all elements of solution array to '\0' using memset
    memset(solution, '\0', boardSize * sideSize);
	//printf("Enter solution lines (press Enter to submit each line, EOF to finish):\n");

    //Loop to read solution lines until EOF or error
    while (fgets(input_line, sizeof(input_line), stdin)) {
        input_line[strcspn(input_line, "\n")] = '\0';
        if (strlen(input_line) != 0) {
           
        
        strcat(solution, input_line);
        strcat(solution, " ");
		int solLeng = strlen(solution);
		printf ("value of solution is %s, len is %d \n", solution, solLeng);
        if (check_solution(boardRead, header, solution) < 0xDEAD) {
            printf("failed");
			return 0;  // Exit on any error
        }
    }
	}
    int ret_val = check_solution(boardRead, header, solution);
    free(header);
    free(boardRead);
    return ret_val;
}

/*
	load board
		open the file using fopen()
		getlines form it?? store it in a 
	load dictionary
	read input
	compare input in solution checker
		
	check if use letter not in board, 
		i need to save board somehow?? <- bucket, 2D array, wierd map??
		go through all chars on the board and check if input has any???
		go through indiv letter user input using getline
		if char not equal to any letter in the bucket then it is no. do loop of the user input letters through board bucket that will 
		increment 1 if found, if counter for this is 0 not found and return/end 
		

	then check if adjacent letters not used 
		if char used in same row?? don't know how do
	
	then check if word in dictionary. 
		compare string in loop of dictionary and if not there say not there

	after check if first last letters the same for the two adjacent words
		first part of word [i] last part of previous input[i-1]	
		WE NEED TO STORE THE PREVIOUS USER INPUTS???	
		store the previous user input in a temp word input if get "correct"

	not all letters used
		how do i do this 		

	if all that is fine return correct
		store the last letter of the user input for the check
	
	else return other error???
		how??????????


*/
