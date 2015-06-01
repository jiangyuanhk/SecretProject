//FILE: peer/peer.c
//
//Description: This file implements a peer in the DartSync project.
//
//Date: May 22, 2015

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <assert.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <errno.h>

#include "peer.h"
#include "file_monitor.h"
#include "../common/constants.h"
#include "../common/pkt.h"
#include "../common/filetable.h"
#include "../common/peertable.h"
#include "peer_helpers.h"
#include "../fileMonitor/fileMonitor.h"


// Globals Variables
int tracker_connection;     // socket connection between peer and tracker so can send / receive between the two
int heartbeat_interval;
int piece_len;
int register_recv = 0;

//terminates the threads when this flips to 0
int noSIGINT = 1;

char* directory;
fileTable_t* filetable;               //local file table to keep track of files in the directory
downloadTable_t* downloadtable;      //peer table to keep track of ongoing downloading tasks

// pthread_mutex_t blockList_mutex;     //block list mutex

//Function to connect the peer to the tracker on the HANDSHAKE Port.
// Returns -1 if it failed to connect.  Otherwise, returns the sockfd
int connect_to_tracker() {
  int tracker_connection;
  struct sockaddr_in servaddr;

  //Get the host name by requesting user input and check if valid
  char ip[IP_LEN];
  printf("Enter ip address of the tracker to connect to: %s", ip);
  scanf("%s",ip);

  // hostInfo = gethostbyname(hostname);
  // if (!hostInfo) {
  //   printf("Error with the hostname of the tracker!\n");
  //   return -1;
  // }
    
  // Set up the tracker address info so we can connect to it
  servaddr.sin_family = AF_INET; 
  servaddr.sin_addr.s_addr = inet_addr(ip);     
  servaddr.sin_port = htons(HANDSHAKE_PORT);

  // Create socket on local host to connect to the tracker
  tracker_connection = socket(AF_INET, SOCK_STREAM, 0);  
  if (tracker_connection < 0) {
    return -1;
  }

  printf("Attempting to connect to the tracker.\n");
  if (connect (tracker_connection, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0){
    return -1;
  }

  printf("Successfully connected to the tracker.\n");
  return tracker_connection; 
}
//http://stackoverflow.com/questions/3284552/how-to-remove-a-non-empty-directory-in-c
//although I found this on stack overflow, it uses techniques I have already implemented elsewhere
void delete_folder_tree (const char* directory_name) {
    DIR* dir;
    struct dirent*  ent;
    char buf[FILE_NAME_MAX_LEN] = {0};

    dir = opendir(directory_name);

    while ((ent = readdir(dir)) != NULL) {
        if( (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0) ) {
          continue;
        }
        sprintf(buf, "%s/%s", directory_name, ent->d_name);
        printf("Deleting entity from the filetable: %s \n", ent -> d_name);

        filetable_deleteFileEntryByName(filetable, ent->d_name);
        struct stat entinfo;
        
        if (S_ISDIR(entinfo.st_mode)) {
            delete_folder_tree(buf);
          }
        
        unlink(buf);
    }

    closedir(dir);
    rmdir(directory_name);
}

//Timer waiting to make sure that the file monitor has processed the changed the file table before removing the block.
void* delete_timer(void* arg) {
  char* filepath = (char*) arg;
  sleep(MONITOR_POLL_INTERVAL);
  unblockFileDeleteListening(filepath);
  // pthread_mutex_unlock(&blockList_mutex);
  pthread_exit(NULL);
}

//Thread to listen for messages from the tracker.  Upon receiving messages from the tracker, it looks
// to sync the local files with the tracker file knowledge, creating download threads as necessary.
void* tracker_listening(void* arg) {
  printf("Start Tracker Listening Thread. \n");
  ptp_tracker_t* pkt = malloc(sizeof(ptp_tracker_t));

  fileTable_t* tracker_filetable = NULL;
  //continuously receive packets from the tracker
  while(pkt_peer_recvPkt(tracker_connection, pkt) > 0) {
    printf("Received a packet from the tracker\n");
    
    //extract the file table from the packet from the tracker
    tracker_filetable = filetable_convertEntriesToFileTable(pkt -> filetableHeadPtr);
    printf("Received filetable:\n");
    filetable_printFileTable(tracker_filetable);
    //loop through the master file table to see which files need to be synchronized locally
    fileEntry_t* file = tracker_filetable -> head;
    
    while (file != NULL) {

      //check to see if the file exists locally
      fileEntry_t* local_file = filetable_searchFileByName(filetable, file -> file_name); 
      
      // download the updated file if the local file does not exist (ADD)
      if(local_file == NULL) {
        // pthread_mutex_lock(&blockList_mutex);
        blockFileAddListening(file -> file_name);
        if(S_ISDIR(file -> file_type)) {
            mkdir(file -> file_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            //add the directory to the local filetable
            fileEntry_t* new_folder = FileEntry_create(file -> file_name);
            filetable_appendFileEntry(filetable, new_folder);
            printf("Directory created: %s\n", file -> file_name);
            sleep(MONITOR_POLL_INTERVAL);
            unblockFileAddListening(file -> file_name);
        }

        else {
          //make sure that the file is not already being downloaded and if not, add to the peer table
          if (search_downloadtable_for_entry(downloadtable, file -> file_name) == NULL) {
            printf("File added. Need to download file: %s\n", file -> file_name);
            pthread_t p2p_download_file_thread;
            pthread_create(&p2p_download_file_thread, NULL, p2p_download_file, file);
          }
        }
      }

      //download the file the file if it outdated (UPDATE)
      else if ( (file -> timestamp) > (local_file -> timestamp) && !S_ISDIR(file -> file_type)) { 
        
        //make sure that the file is not already being downloaded and if not, add to the peer table
        if (search_downloadtable_for_entry(downloadtable, file -> file_name) == NULL) {
          // pthread_mutex_lock(&blockList_mutex);
          blockFileWriteListening(file -> file_name);
          printf("File updated.  Need to download file: %s\n", file -> file_name);
          pthread_t p2p_download_file_thread;
          pthread_create(&p2p_download_file_thread, NULL, p2p_download_file, file);
        }

      }

      file = file -> next;  //move to next item in file table from tracker
    }

    printf("Finished determining files that need to be uploaded.\n");
    //Delete files that are in local table, but not in the file table from the tracker
    file = filetable -> head;
    while (file != NULL) {

      //check to see if the local file exists in the tracker table
      fileEntry_t* master_file = filetable_searchFileByName(tracker_filetable, file -> file_name);

      char delete_filepath [FILE_NAME_MAX_LEN];
      memset(delete_filepath, 0, FILE_NAME_MAX_LEN);

      //if the file is not in the master file table and is locally, then delete it
      if (master_file == NULL) {
        // pthread_mutex_lock(&blockList_mutex);
        blockFileDeleteListening(file -> file_name);

        memcpy(delete_filepath, file -> file_name, strlen(file -> file_name));

        if(S_ISDIR(file -> file_type)) {
          delete_folder_tree(file -> file_name);
          printf("Successfully removed the directory in the filesystem: %s \n", file -> file_name);
        }

        else if( remove(file -> file_name) == 0) {
          printf("Successfully removed the file in filesystem: %s \n", file -> file_name);
        }

        else {
          printf("Error in removing the file from the file system.\n");
        }          
      }

      file = file -> next;

      //remove the file from the filetable if it was removed
      if (strlen(delete_filepath) > 0) {
        printf("Deleting the file from the filetable\n");
        filetable_deleteFileEntryByName(filetable, delete_filepath);
        pthread_t delete_timer_thread;
        pthread_create(&delete_timer_thread, NULL, delete_timer, delete_filepath);
      }

    }
    printf("Finished deleting files.\n");
  }

  free(pkt);
  if (tracker_filetable) filetable_destroy(tracker_filetable);
  pthread_exit(NULL);
}


// Thread to listen on the P2P port for download requests from other peers to spawn upload threads.
// First initializes a port to listen for requests to connect to it and then listens.
void* p2p_listening(void* arg) {
  printf("Start p2p listening thread. \n");
  //Set up the socket for listening
  int peer_sockfd;
  struct sockaddr_in local_peer_addr;

  struct sockaddr_in other_peer_addr;
  socklen_t other_peer_addr_len;

  peer_sockfd = socket(AF_INET, SOCK_STREAM, 0); 
  if (peer_sockfd < 0) {
    pthread_exit(NULL);
  }

  //Set up the local addr info
  memset(&local_peer_addr, 0, sizeof(local_peer_addr));
  local_peer_addr.sin_family = AF_INET;
  local_peer_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  local_peer_addr.sin_port = htons(P2P_PORT);

  //Attempt to bind the socket
  if (bind (peer_sockfd, (struct sockaddr *)&local_peer_addr, sizeof(local_peer_addr)) < 0){
    printf("Failed to bind.\n");
    pthread_exit(NULL);
  }

  //Attempt to set up socket for listening
  if (listen (peer_sockfd, MAX_NUM_PEERS) < 0) {
    printf("Failed to listen.\n");
    pthread_exit(NULL);
  }
  
  //Start infinite loop waiting to accept connections from peers.
  //When a peer connects, we create a upload thread to handle that connection.  
  while(noSIGINT) {
    int peer_conn;

    printf("Listening for a connection.\n");
    peer_conn = accept(peer_sockfd, (struct sockaddr*) &other_peer_addr, &other_peer_addr_len);
    if (peer_conn < 0){
      printf("Error connecting with download thread.\n");
      continue;
    }

    printf("Established a connection to a peer. Starting a P2PDownload Thread.\n");

    //start the p2p_download_thread
    pthread_t p2p_upload_thread;
    pthread_create(&p2p_upload_thread, NULL, p2p_upload, &peer_conn);
  }

  if (peer_sockfd != -1) {
    close(peer_sockfd);
  }

  pthread_exit(NULL); 
}


//Function that downloads p2p pieces until there are no more for the file.
void* p2p_download(void* arg) {
  
  //Read in the information from the struct and make local variables, freeing the struct
  arg_struct_t* args = (arg_struct_t*) arg;
  downloadEntry_t* entry = args -> download_entry;
  char ip[IP_LEN];
  memset(ip, 0, IP_LEN);
  memcpy(ip, args -> ip, strlen(args->ip) );
  free(args);
  
  printf("Trying to connect to the download thread for ip: %s and file: %s\n", ip, entry -> file_name);

  // Create a socket and connect to the given IP address.
  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = inet_addr(ip);      
  servaddr.sin_port = htons(P2P_PORT);

  int peer_conn = socket(AF_INET, SOCK_STREAM, 0);  

  if(peer_conn < 0) {
    printf("Error creating socket in p2p download.\n");
    pthread_exit(NULL);
  }

  if( connect(peer_conn, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0){
    printf("Failed to connect in P2P process.\n");
    pthread_exit(NULL);
  }

  printf("Connected to a Upload Thread for IP: %s and file: %s\n", ip, entry -> file_name);

  //keep polling the entry list trying to get an entry to receive.
  while(entry -> successful_pieces != entry -> num_pieces) {

    downloadPiece_t* piece;
    if ( (piece = get_downloadPiece(entry)) != NULL) {
      
      printf("Piece to Request Download from IP %s. Start : %d Size: %d Piece Num: %d \n", ip, piece -> start, piece -> size, piece -> piece_num);
      file_metadata_t* metadata;
      
      // if sending the metadata failed, we cannot send the piece so readd the piece to the piece list
      if ( (metadata = send_meta_data_info(peer_conn, entry -> file_name, piece -> start, piece -> size, piece -> piece_num)) == NULL) {
        printf("%s: Readding the piece to the piece list. Try again for piece.\n", ip);
        readd_piece_to_list(entry, piece);
        continue;
        // close(peer_conn);
        // pthread_exit(NULL);
      }

      //if we fail receiving the data from the peer, re-add the piece to the folder
      if (receive_data_p2p(peer_conn, metadata) < 0) {
        printf("Error receiving the data p2p.  Re-adding the piece to the list.\n");
        readd_piece_to_list(entry, piece);
        free(metadata);
        continue;
        // close(peer_conn);
        // pthread_exit(NULL);
      }

      printf("Received a piece. Num: %d\n", piece -> piece_num);

      free(metadata);

      entry -> successful_pieces ++;
    }
  }
  printf("Finished receiving pieces from the ip: %s\n", ip);

  close(peer_conn);
  pthread_exit(0);
}

void* p2p_download_file(void* arg) {

  fileEntry_t* file = (fileEntry_t*) arg;

  downloadEntry_t* entry = init_downloadEntry(file, piece_len);
  add_entry_to_downloadtable(downloadtable, entry);
  printf("Add File to Download Table: %s\n", file -> file_name);

  // this is the download file thread that should spin off
  int i = 0;
  while (strlen(file -> iplist[i]) != 0) {
    printf("Creating list download thread for ip: %s for downloading file: %s\n", file -> iplist[i], entry -> file_name);

    arg_struct_t* args = create_arg_struct(entry, file -> iplist[i]);
    
    pthread_t p2p_download_thread;
    pthread_create(&p2p_download_thread, NULL, p2p_download, args);
    i++;
  }

  //wait until the pieces are all successfully downloaded
  printf("Waiting for the pieces to all be received for file: %s\n", entry -> file_name);
  while(entry -> successful_pieces != entry -> num_pieces);
  printf("All of the pieces have been received for file: %s\n", entry -> file_name);
 
  if (recombine_temp_files(entry -> file_name, entry -> num_pieces) < 0) {
    printf("Error recombining the temp files.  Could not updated the entry. \n");
  }

  else {
    printf("File Succesfully Updated: %s\n", entry -> file_name);
  }

  // Set the last modify time to be that from the tracker filetable info
  printf("Updated file time of %s to reflect timestamp in tracker\n", entry -> file_name);
  struct utimbuf* newTime = (struct utimbuf*) malloc(sizeof(struct utimbuf));
  newTime -> modtime = file -> timestamp;
  utime(entry -> file_name, newTime);

  fileEntry_t* old_entry = filetable_searchFileByName(filetable, entry -> file_name);

  //if the old entry exists, it is a modfy so update
  if (old_entry) {
    printf("Update the file entry and unblock the entry.\n");
    filetable_updateFile(old_entry, file, filetable -> filetable_mutex);
    sleep(MONITOR_POLL_INTERVAL);
    unblockFileWriteListening(entry -> file_name);
  }

  else {
    printf("Add the file entry to the filetable and unblock the entry.\n");
    fileEntry_t* new_file = FileEntry_create(entry -> file_name);
    filetable_appendFileEntry(filetable, new_file);
    sleep(MONITOR_POLL_INTERVAL);
    unblockFileAddListening(entry -> file_name);
  }

  //remove this from the peer table so know that the download is completed
  printf("Remove the entry for file %s from the downloadtable\n", file -> file_name);
  remove_entry_from_downloadtable(downloadtable, file -> file_name);
  printf("Closing download file thread. \n");
  // if (file) free(file);
  



  pthread_exit(NULL);
}

void* p2p_upload(void* arg) {
  int peer_conn = *(int*) arg;

  printf("Begin upload.\n");
  
  while(1) {
    //get the filename of the file to send  
    file_metadata_t* recv_metadata = calloc(1,sizeof(file_metadata_t));
    
    if (receive_meta_data_info(peer_conn, recv_metadata) < 0){
      printf("Error receiving metadata.  Exiting upload thread.\n");
      free(recv_metadata);
      pthread_exit(NULL);
    }

    printf("Received the meta data info in upload thread.\n");

    //if the filename is not there, done uploading.
    if (recv_metadata ->filename[0] == 0) {
      printf("Error receiving metadata for the file\n");
      free(recv_metadata);
      pthread_exit(NULL);
    }

    printf("Metadata. Name: %s, Size: %d, Start: %d\n", recv_metadata -> filename, recv_metadata -> size, recv_metadata -> start);

    //Sending a file p2p
    printf("Attempting to send a file: %s \n", recv_metadata -> filename);
    
    if (send_data_p2p(peer_conn, recv_metadata) < 0) {
      printf("Error sending data p2p.  Exiting upload thread.\n");
      free(recv_metadata);
      pthread_exit(NULL);
    }

    free(recv_metadata);
  }
  
  printf("End upload.\n");
  pthread_exit(NULL);
}

void* keep_alive(void* arg) {
  //int interval = *(int*) arg;
  ptp_peer_t* pkt = pkt_create_peerPkt();
  char ip_address[IP_LEN];
  get_my_ip(ip_address);
  pkt_config_peerPkt(pkt, KEEP_ALIVE, ip_address, HANDSHAKE_PORT, 0, NULL);
  while(noSIGINT) {
    pkt_peer_sendPkt(tracker_connection, pkt, NULL);
    sleep(heartbeat_interval);
  }
  free(pkt);
  pthread_exit(NULL);
}

//--------------------File Monitor Callbacks-------------------------
/*
*Sends the peers filetable to the tracker
*/
void Peer_sendfiletable() {
  printf("Attempting to send the file table.\n");
  pthread_mutex_lock(filetable -> filetable_mutex);
  ptp_peer_t* pkt = pkt_create_peerPkt();
  char ip_address[IP_LEN];
  get_my_ip(ip_address);
  pkt_config_peerPkt(pkt, FILE_UPDATE, ip_address, HANDSHAKE_PORT, filetable -> size, filetable -> head);
  pthread_mutex_unlock(filetable -> filetable_mutex);
  pkt_peer_sendPkt(tracker_connection, pkt, filetable -> filetable_mutex);
  printf("Successfully send the filetable packet.\n");
}
/*
* Creates a fileEntry_t from a file name
*
*@name: name to return
*/
fileEntry_t* FileEntry_create(char* name) {
    char* filepath;
    if(strncmp(name, directory, strlen(directory)) != 0) {
      filepath = calloc(1, (strlen(directory) + strlen(name) + 1) * sizeof(char));
      sprintf(filepath, "%s%s", directory, name);
    }
    else {
      //filepath = name;
      filepath = calloc(1, strlen(name) + 1);
      strcpy(filepath, name);
    }

  FileInfo myInfo = getFileInfo(filepath);

  fileEntry_t* newEntryPtr = filetable_createFileEntry(filepath, myInfo.size, myInfo.lastModifyTime, myInfo.type);

  free(myInfo.filepath);
  free(filepath);

  return newEntryPtr;
  
}
/* 
* Callback add method for the File Monitor
*@name: name of the file to add
*/
void Filetable_peerAdd(char* name) {
  fileEntry_t* newEntryPtr = FileEntry_create(name);
  filetable_appendFileEntry(filetable, newEntryPtr);
  printf("File entry for %s added to filetable\n", name);
  //Peer_sendfiletable();
}

/* 
* Callback update method for the File Monitor
*@name: name of the file to update
*/
void Filetable_peerModify(char* name) {
  char* filepath = calloc(1, (strlen(directory) + strlen(name) + 1) * sizeof(char));
  sprintf(filepath, "%s%s", directory, name);
  fileEntry_t* oldEntryPtr = filetable_searchFileByName(filetable, filepath);
  fileEntry_t* newEntryPtr = FileEntry_create(name);
  int ret = filetable_updateFile(oldEntryPtr, newEntryPtr, filetable->filetable_mutex);

  if (ret) {
    printf("File entry for %s modified\n", name);
    //Peer_sendfiletable();
  }
  else {
    printf("Update failed: File entry for %s not found\n", name);
  }
  free(filepath);
}
/* 
* Callback delete method for the File Monitor
*@name: name of the file to delete
*/
void Filetable_peerDelete(char* name) {
  int ret = filetable_deleteFileEntryByName(filetable, name);
  if (ret) {
    printf("File entry for %s deleted\n", name);
    //Peer_sendfiletable();
  }
  else {
    printf("File entry for %s not found\n", name);
  }
}
/* 
* Callback sync method for the File Monitor
*@name: name of the file to add
*/
void Filetable_peerSync() {
  //Receive the acknowledgement of the register packet from the tracker
  printf("Receiving registered handshake packet from the tracker.\n");
  ptp_tracker_t* packet = calloc(1, sizeof(ptp_tracker_t));
  register_recv = pkt_peer_recvPkt(tracker_connection, packet);
  heartbeat_interval = packet -> heartbeatinterval;
  piece_len = packet -> piece_len;  
  printf("Interval: %d  Piece Len: %d   FT-Size: %d \n", packet -> heartbeatinterval, packet -> piece_len, packet -> filetablesize);

  //extract the file table from the packet from the tracker
  fileTable_t* tracker_filetable = filetable_convertEntriesToFileTable(packet -> filetableHeadPtr);
  printf("Received filetable:\n");
  filetable_printFileTable(tracker_filetable);
  //loop through the master file table to see which files need to be synchronized locally
  fileEntry_t* file = tracker_filetable -> head;
    
  while (file != NULL) {

    //check to see if the file exists locally
    fileEntry_t* local_file = filetable_searchFileByName(filetable, file -> file_name); 
      
    // download the updated file if the local file does not exist (ADD)
    if(local_file == NULL) {
      // pthread_mutex_lock(&blockList_mutex);
      blockFileAddListening(file -> file_name);
      if(S_ISDIR(file -> file_type)) {
          mkdir(file -> file_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
          //add the directory to the local filetable
          fileEntry_t* new_folder = FileEntry_create(local_file -> file_name);
          filetable_appendFileEntry(filetable, new_folder);
          printf("Directory created: %s\n", file -> file_name);
      }
      else {
        printf("File added. Need to download file: %s\n", file -> file_name);
        pthread_t p2p_download_thread;
        pthread_create(&p2p_download_thread, NULL, p2p_download_file, file);
      }
    }

    //download the file the file if it outdated (UPDATE)
    else if ( (file -> timestamp) > (local_file -> timestamp) && !S_ISDIR(file -> file_type)) {
      printf("File updated or added.  Need to download file: %s\n", file -> file_name);
      // pthread_mutex_lock(&blockList_mutex);
      blockFileWriteListening(file -> file_name);
        
      //make sure that the file is not already being downloaded and if not, add to the peer table
      pthread_t p2p_download_file_thread;
      pthread_create(&p2p_download_file_thread, NULL, p2p_download_file, file);
    }

    file = file -> next;  //move to next item in file table from tracker
  }

  printf("Finished determining files that need to be synced.\n");
  free(packet);
  printf("Peer synced to tracker\n");
}
//--------------------File Monitor Callbacks-------------------------
/*
* Stops and cleans up the peer
*/
void peer_stop() {
  noSIGINT = 0;
  FileMonitor_close();
  close(tracker_connection);
  filetable_destroy(filetable);
  downloadtable_destroy(downloadtable);
  free(directory);
  exit(0);
}


int main(int argc, char *argv[]) {

  //register a signal handler which is used to terminate the process
  signal(SIGINT, peer_stop);

  //Initialize the directory to sync
  printf("Read in config file to get directory.\n");
  directory = readConfigFile("config");

  if (directory == NULL) {
    printf("Error reading the config file.\n");
    exit(1);
  }

  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
    printf("Current working dir: %s\n", cwd);
  
  printf("Successfully read in config file.\n");

  //Create the local file table
  //filetable = create_local_filetable(directory);
  filetable = filetable_init();
  //filetable_printFileTable(filetable);
  //Initialize the peer table as empty
  downloadtable = init_downloadTable();

  //Attempt to establish connection with tracker
  if ( (tracker_connection = connect_to_tracker()) < 0) {
    printf("Failed to connect to tracker. Exiting\n");
    exit(0);
  }


  printf("Connected\n");

  //Send a register packet
  ptp_peer_t* register_pkt = pkt_create_peerPkt();
  char* ip = malloc(IP_LEN);
  memset(ip, 0, IP_LEN);
  get_my_ip(ip);
  pkt_config_peerPkt(register_pkt, REGISTER, ip, P2P_PORT, 0, NULL);
 
  if (pkt_peer_sendPkt(tracker_connection, register_pkt, NULL) < 0){
    printf("Failed to send register packet\n");
    free(ip);
    free(register_pkt);
    return -1; // maybe exit();
  }
  free(ip);
  free(register_pkt);
  printf("Successfully sent register packet.\n");
  

  //--------------------File Monitor Thread-------------------------
  void (*Add)(char *);
  void (*Modify)(char *);
  void (*Delete)(char *);
  void (*Sync)(void);
  void (*SendUpdate)(void);

  Add = &Filetable_peerAdd;
  Modify = &Filetable_peerModify;
  Delete = &Filetable_peerDelete;
  Sync = &Filetable_peerSync;
  SendUpdate = &Peer_sendfiletable;

  localFileAlerts myFuncs = {
    Add,
    Modify,
    Delete,
    Sync,
    SendUpdate
  };

  // pthread_mutex_init(&blockList_mutex, NULL);

  pthread_t monitorthread;
  pthread_create(&monitorthread, NULL, fileMonitorThread, (void*) &myFuncs);

  //--------------------File Monitor Thread-------------------------


  char cwd1[1024];
  if (getcwd(cwd1, sizeof(cwd1)) != NULL)
    printf("Current working dir: %s\n", cwd1);

  while(!register_recv) {
    sleep(MONITOR_POLL_INTERVAL);
  }
  //printf("Begin initial sync with the peer. Send file table to peer.\n");
  printf("Begin the tracker listening thread. \n");
  // start the thread to listen for data from the tracker
  pthread_t tracker_listening_thread;
  pthread_create(&tracker_listening_thread, NULL, tracker_listening, (void*)0);


  // start the thread to listen on the p2p port for connections from other peers
  pthread_t p2p_listening_thread;
  pthread_create(&p2p_listening_thread, NULL, p2p_listening, &tracker_connection);

  //Start the keep alive thread
  pthread_t keep_alive_thread;
  pthread_create(&keep_alive_thread, NULL, keep_alive, NULL);

  while(noSIGINT) sleep(60);
}
