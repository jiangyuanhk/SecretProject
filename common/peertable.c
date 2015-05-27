#include "peertable.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/time.h>
#include "../common/utils.h"


/* Function to initialize a peer table.  The head and tail of the peer table will be set to 
	NULL and the size is 0.

	@return the pointer to the peerTable_t* that is created.
*/
peerTable_t* peertable_init(){
	peerTable_t *peertable = (peerTable_t*) malloc(sizeof(peerTable_t));
	peertable->head = NULL;
	peertable->tail = NULL;
	peertable->size = 0;
	return peertable;
}


/**
 * Create a peertable entry from the given sockfd and ip.
 * @param  head      [head of fileEntries in dest fileTable]
 * @param  filename  [filename]
 * @return           [pointer to entry if found, NULL if cannot find]
 */
peerEntry_t* peertable_createEntry(char* ip, int sockfd){
	// Allocate memory for the entry.
  peerEntry_t* peerEntry = (peerEntry_t*)malloc(sizeof(peerEntry_t));

  // Set initial fields for the entry.
  memcpy(peerEntry->ip, ip, IP_LEN);
  peerEntry->sockfd = sockfd;
  peerEntry->timestamp = getCurrentTime();
  peerEntry->next = NULL;

	return peerEntry;
}

/**
 * Adds a new peer entry to the end of the peer Table.
 * @param  table  [pointer to the peer Table]
 * @param  entry  [pointer to the peer entry to add]
 */
int peertable_addEntry(peerTable_t *table, peerEntry_t* entry) {

  // when table is empty, add the entry and make it the head and tail
  if (table -> size == 0) {
  	table -> head = entry;
  	table -> tail = entry;
  }

  // otherwise, append the new entry after the tail and make it the new tail
  else {
    table -> tail -> next = entry;
    table -> tail = entry;
  }

  table -> size = table -> size + 1;
 	return 1;
}




/**
 * Removes a table entry given the IP addressof the node to delete.
 * Also, updates the necessary pointers upon deletion.
 * @param  table  [pointer to the peer Table]
 * @param  ip     [pointer to ip address to delete]
 * @return [returns 1 on success, -1 on failure]
 */
int peertable_deleteEntryByIp(peerTable_t *table, char* ip) {


	if(table->size == 0) return -1; //empty table

	pthread_mutex_lock(table->peertable_mutex);
	peerEntry_t* dummy = (peerEntry_t*)malloc(sizeof(peerEntry_t));
	dummy->next = table->head;
	peerEntry_t* iter = dummy;

	while(iter->next != NULL){
		if(strcmp(iter->next->ip, ip) == 0){

			peerEntry_t* temp = iter->next;
			iter->next = iter->next->next;
			free(temp);
			free(dummy);
			table->size -= 1;
			pthread_mutex_unlock(table->peertable_mutex);
			return 1;
		}
		iter = iter->next;
	}

	free(dummy);
	pthread_mutex_unlock(table->peertable_mutex);
	return -1; // not found

}




void peertable_destroy(peerTable_t *table) {
	
	//free entry if any
	if(table->size != 0){
		pthread_mutex_lock(table->peertable_mutex);
		peerEntry_t* iter = table->head;
		while(iter){
			peerEntry_t* cur = iter;
			iter = iter->next;
			free(cur);
		}
		pthread_mutex_unlock(table->peertable_mutex);
	}
	//free table mutex
	free(table->peertable_mutex);
	return;
}





// check if the table has a peer with the same IP as the peer
int peertable_existPeer(peerTable_t *table, peerEntry_t* entry){

	if(table->size == 0) return -1;
	pthread_mutex_lock(table->peertable_mutex);
	peerEntry_t* iter = table->head;
	while(iter != NULL){
		if(strcmp(entry->ip, iter->ip) == 0){
			pthread_mutex_unlock(table->peertable_mutex);
			return 1;
		}
		iter = iter->next;
	}
	pthread_mutex_unlock(table->peertable_mutex);
	return -1;
}




/**
 * search the peerTable to find the peerEntry identified by ip
 * return it if found, return NULL if not
 */

peerEntry_t* peertable_searchEntryByIp(peerTable_t* table, char* ip){
	
	if(table->size == 0) return NULL;
	pthread_mutex_lock(table->peertable_mutex);
	peerEntry_t* iter = table->head;
	while(iter != NULL){
		if(strcmp(iter->ip, ip) == 0) {
			pthread_mutex_unlock(table->peertable_mutex);
			return iter;
		}
		iter = iter->next;
	}

	pthread_mutex_unlock(table->peertable_mutex);
	return iter;

	
}


/**
 * refresh the peerEntry's timestamp to latest time
 * @param  entry [description]
 * @return       [description]
 */
int peertable_refreshTimestamp(peerEntry_t* entry){
		
	unsigned long curTime = getCurrentTime();
	if(entry->timestamp < curTime) return -1;
	
	entry->timestamp = curTime;
	return 1;
}






