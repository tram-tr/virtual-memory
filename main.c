
/*
Main program for the virtual memory project.
Make all of your modifications to this file.
You may add or rearrange any code or data as you need.
The header files page_table.h and disk.h explain
how to use the page table and disk interfaces.
*/

#include "page_table.h"
#include "disk.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int npages, nframes;
int disk_reads, disk_writes, page_faults;
unsigned char *virtmem = NULL;
unsigned char *physmem = NULL;
struct disk *disk = NULL;
int *free_frames = NULL; // keep track of the availability of the frames in the physical memory
int *frames_info = NULL; // stores the page number corresponding to each frame in the physical memory

typedef enum {
    FIFO,
    RAND,
    CUSTOM
} algo;
algo page_algo;

typedef struct node_t {
    int id;
    int lru; // for custom algo only
    struct node_t* next;
} node_t;

// fifo
node_t* fifo_head = NULL;
node_t* fifo_tail = NULL;

// custom
node_t* lru_head = NULL;
node_t* lru_tail = NULL;

/* linked list functions start here */
node_t* create_node(int node) {
    node_t* new_node = (node_t*)malloc(sizeof(node_t));
    new_node->id = node;
    new_node->lru = 1;
    new_node->next = NULL;
    return new_node;
}

void add_node(node_t** head, node_t** tail, int node) {
    node_t* new_node = create_node(node);
    if (*head == NULL) {
        *head = new_node;
        *tail = new_node;
    } else {
        (*tail)->next = new_node;
        *tail = new_node;
    }
}

int remove_first_node(node_t** head) {
    if (*head == NULL)
        return -1;
    node_t* temp = *head;
    int removed = temp->id;
    *head = (*head)->next;
    free(temp);
    return removed;
}

void free_linked_list(node_t** head) {
    node_t *tmp;
    while (*head != NULL) {
        tmp = *head;
        *head = (*head)->next;
        free(tmp);
    }
}

void dump_list(node_t* head) {
    node_t* curr = head;
    while (curr != NULL) {
        printf("frame %d, freq %d\n", curr->id, curr->lru);
        curr = curr->next;
    }
}
/* linked list functions end here */

/* handling page faults functions start here */
void init_frames() {
    if (nframes > 0) {
        free_frames = calloc(nframes, sizeof(int));
        frames_info = calloc(nframes, sizeof(int));
    }
    else {
        fprintf(stderr,"couldn't create frames: %s\n",strerror(errno));
        exit(1);
    }
}

int select_free_frame() {
    for (int i = 0; i < nframes; i++) {
        if (free_frames[i] == 0) {
            free_frames[i] = 1;
            return i;
        }
    }
    return -1;
}

void update_access(node_t** head, int node) {
    node_t* curr = *head;
    while (curr != NULL) {
        if (curr->id != node) 
            curr->lru++;
        else 
            curr->lru = 1;
        curr = curr->next;
    }
}

void replace_page(struct page_table *pt, int page, int removed_frame_id) {
    int removed_page = frames_info[removed_frame_id];
    int removed_frame, removed_bits;
    page_table_get_entry(pt, removed_page, &removed_frame, &removed_bits);

    // check for dirty bit
    if ((removed_bits & PROT_WRITE) != 0) {
        disk_write(disk, removed_page, &physmem[removed_frame * PAGE_SIZE]);
        disk_writes++;
    }

    // replace page
    //printf("remove page %d with %d at frame %d and %d\n", removed_page, page, removed_frame, removed_frame_id);
    page_table_set_entry(pt, removed_page, 0, 0);
    page_table_set_entry(pt, page, removed_frame, 0 | PROT_READ);
    disk_read(disk, page, &physmem[removed_frame * PAGE_SIZE]);
    disk_reads++;
    frames_info[removed_frame] = page;
}

void fifo_replace_page(struct page_table *pt, int page) {
    int removed_frame_id = remove_first_node(&fifo_head);
    if (removed_frame_id < 0) {
        printf("couldn't replace page\n");
        exit(1);
    }
    replace_page(pt, page, removed_frame_id);

    // circular linked list
    add_node(&fifo_head, &fifo_tail, removed_frame_id);
}

void lru_replace_page(struct page_table *pt, int page) {
    node_t *curr = lru_head;
    node_t *lru_frame = NULL;

    // find the least recently used
    while (curr != NULL) {
        if (lru_frame == NULL || curr->lru > lru_frame->lru) 
            lru_frame = curr;
        curr = curr->next;
    }

    int removed_frame_id = lru_frame->id;
    replace_page(pt, page, removed_frame_id);
    //add_node(&lru_head, &lru_tail, removed_frame);
    update_access(&lru_head, removed_frame_id);
    //dump_list(lru_head);
}

void rand_replace_page(struct page_table *pt, int page) {
    int removed_frame_id = (int)lrand48() % nframes;
    replace_page(pt, page, removed_frame_id);
}

void page_fault_handler(struct page_table *pt, int page) {
    page_faults++;
    // get current frame and permission bits of the page
    int curr_bits, curr_frame;
    page_table_get_entry(pt, page, &curr_frame, &curr_bits);

    /* page fault due to page not in physical memory */
    if ((curr_bits & PROT_READ) == 0 && (curr_bits & PROT_WRITE) == 0) {
        // select a free frame
        int frame = select_free_frame();

        // if there is no available frame
        if (frame < 0) {
            if (page_algo == FIFO)
                fifo_replace_page(pt, page);
            else if (page_algo == RAND)
                rand_replace_page(pt, page);
            else if (page_algo == CUSTOM) {
                //printf("page fault at page %d, no free frame\n", page);
                lru_replace_page(pt,page);
            }
        }
        // if there is free frame
        else {
            // update page table, map page to frame with read permission
            page_table_set_entry(pt, page, frame, 0 | PROT_READ);

            if (page_algo == FIFO)
                add_node(&fifo_head, &fifo_tail, frame);
            else if (page_algo == CUSTOM) {
                //printf("page fault at page %d, mapping to free frame %d\n", page, frame);
                add_node(&lru_head, &lru_tail, frame);
				update_access(&lru_head, frame);
                //dump_list(lru_head);
			}

            // read data from disk to frame
            disk_read(disk, page, &physmem[frame * PAGE_SIZE]);
            disk_reads++;
            frames_info[frame] = page;
        }

    }
    /* page fault due to page is missing permission */
    else {
        if ((curr_bits & PROT_READ) == 1 && (curr_bits & PROT_WRITE) == 0)
            page_table_set_entry(pt, page, curr_frame, curr_bits | PROT_WRITE);
        else 
            page_table_set_entry(pt, page, curr_frame, curr_bits | PROT_READ);
		if (page_algo == CUSTOM) {
            //printf("page fault for missing permission at page %d at current frame %d\n", page, curr_frame);
            update_access(&lru_head, curr_frame);
            //dump_list(lru_head);
        }
    }
    //printf("page fault on page #%d\n",page);
    //page_table_set_entry(pt, page, page, PROT_READ|PROT_WRITE);

}
/* handling page faults functions end here */

/* functions support generating data for the report start here */
void write_to_csv(const char *filename, const char *program, const char *algo) {
    int csv_file = open(filename, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (csv_file < 0) {
        fprintf(stderr, "Failed to open or create CSV file: %s\n", strerror(errno));
        exit(1);
    }

    dprintf(csv_file, "%s,%s,%d,%d,%d,%d,%d\n", program, algo, npages, nframes, page_faults, disk_reads, disk_writes);
    close(csv_file);
}
/* functions support generating data from the report end here */

/* main program that support generating data for the report */
/*int main( int argc, char *argv[] ) {
    if(argc!=4) {
        printf("use: virtmem <nframes_start> <nframes_end> <alpha|beta|gamma|delta>\n");
        return 1;
    }

    for (int i = 0; i < 3; i++) {
        const char *program = argv[3];
        char csv_filename[256];

        if (i == 0) {
            page_algo = FIFO;
            snprintf(csv_filename, sizeof(csv_filename), "report_data/%s_fifo.csv", program);
        }
        if (i == 1) {
            page_algo = RAND;
            snprintf(csv_filename, sizeof(csv_filename), "report_data/%s_rand.csv", program);
        }
        if (i == 2) {
            page_algo = CUSTOM;
            snprintf(csv_filename, sizeof(csv_filename), "report_data/%s_custom.csv", program);
        }

        if (access(csv_filename, F_OK) != -1) {
            // file exists, remove it
            if (remove(csv_filename) != 0)
                printf("error removing csv file.\n");
        } 

        for (nframes = atoi(argv[1]); nframes <= atoi(argv[2]); nframes++) {
            npages = 100;
            printf("-------------------------------\n");
            printf("npages = %d, nframes = %d\n", npages, nframes);

            disk = disk_open("myvirtualdisk",npages);
            if(!disk) {
                fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
                return 1;
            }

            // initalize frames
            init_frames();

            struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
            if(!pt) {
                fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
                return 1;
            }

            virtmem = page_table_get_virtmem(pt);
            physmem = page_table_get_physmem(pt);

            disk_reads = disk_writes = page_faults = 0;

            if(!strcmp(program,"alpha")) {
                alpha_program(virtmem,npages*PAGE_SIZE);

            } else if(!strcmp(program,"beta")) {
                beta_program(virtmem,npages*PAGE_SIZE);

            } else if(!strcmp(program,"gamma")) {
                gamma_program(virtmem,npages*PAGE_SIZE);

            } else if(!strcmp(program,"delta")) {
                delta_program(virtmem,npages*PAGE_SIZE);

            } else {
                fprintf(stderr,"unknown program: %s\n",argv[3]);
                return 1;
            }

            if (page_algo == FIFO)
                write_to_csv(csv_filename, program, "fifo");
            else if (page_algo == RAND)
                write_to_csv(csv_filename, program, "rand");
            else 
                write_to_csv(csv_filename, program, "custom");

            free(free_frames);
            free(frames_info);

            if (page_algo == FIFO)
                free_linked_list(&fifo_head);
            if (page_algo == CUSTOM)
                free_linked_list(&lru_head);

            page_table_delete(pt);
            disk_close(disk);
        }
    }

    return 0;
}*/

/* main function */
int main( int argc, char *argv[] ) {
    if(argc!=5) {
        printf("use: virtmem <npages> <nframes> <rand|fifo|custom> <alpha|beta|gamma|delta>\n");
        return 1;
    }

    npages = atoi(argv[1]);
    nframes = atoi(argv[2]);
    
    if (strcmp(argv[3], "fifo") == 0)
        page_algo = FIFO;
    else if (strcmp(argv[3], "rand") == 0)
        page_algo = RAND;
    else if (strcmp(argv[3], "custom") == 0)
        page_algo = CUSTOM;
    else {
        printf("please selec algorithm <rand|fifo|custom>\n");
        return 1;
    }

    const char *program = argv[4];

    disk = disk_open("myvirtualdisk",npages);
    if(!disk) {
        fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
        return 1;
    }

    // initalize frames
    init_frames();

    struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
    if(!pt) {
        fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
        return 1;
    }

    virtmem = page_table_get_virtmem(pt);
    physmem = page_table_get_physmem(pt);

    disk_reads = disk_writes = page_faults = 0;
    if(!strcmp(program,"alpha")) {
        alpha_program(virtmem,npages*PAGE_SIZE);

    } else if(!strcmp(program,"beta")) {
        beta_program(virtmem,npages*PAGE_SIZE);

    } else if(!strcmp(program,"gamma")) {
        gamma_program(virtmem,npages*PAGE_SIZE);

    } else if(!strcmp(program,"delta")) {
        delta_program(virtmem,npages*PAGE_SIZE);

    } else {
        fprintf(stderr,"unknown program: %s\n",argv[4]);
        return 1;
    }
    printf("\npage faults: %d\n", page_faults);
    printf("disk reads: %d\n", disk_reads);
    printf("disk writes: %d\n", disk_writes);

    free(free_frames);
    free(frames_info);

    if (page_algo == FIFO)
        free_linked_list(&fifo_head);
    if (page_algo == CUSTOM)
        free_linked_list(&lru_head);

    page_table_delete(pt);
    disk_close(disk);
    return 0;
}