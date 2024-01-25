//////////////////
// Good stuff to read:        https://beej.us/guide/bgnet/html/#system-calls-or-bust
// Qemu documentation:        https://qemu.readthedocs.io/en/latest/system/devices/net.html
// Builtroot documentation:   https://buildroot.org/downloads/manual/manual.html#configure
// Init scripts documentatio: http://man7.org/linux/man-pages/man8/start-stop-daemon.8.html
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <errno.h>

#include <syslog.h>

#include <signal.h>
#include <pthread.h>

#include "queue.h"
#include "time.h"

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>

/*
int getaddrinfo(const char *node,     // e.g. "www.example.com" or IP
                const char *service,  // e.g. "http" or port number
                const struct addrinfo *hints,
                struct addrinfo **res);*/
                
#define   PORT               "9000"
#define   MAXDATASIZE        1024
#define   BACKLOG            10
#define   FILE_TO_READWRITE  "/var/tmp/aesdsocketdata"

bool caught_sigint  = false;
bool caught_sigterm = false;

char* fileToWrite = FILE_TO_READWRITE;     // Global variable specifying the file to store 
                                           // temporally the data received
int   my_socket   = 0;                     // Global variable to store the file descriptor
                                           // to the socket for receiving data
FILE* filePointer = NULL;                  // File pointer to the file to store temporally
                                           // data received
                                                                                     
struct slist_threadData_s {
  pthread_t            theThreadId;        // Thread Id of the thread.
  bool                 threadCompleted;    // Thread execution has been completed
  bool                 threadDeleted;      // The cread has been deleted (called thread_join)
  
  struct sockaddr_in   their_addr;
  int                  new_socket;
  
  SLIST_ENTRY(slist_threadData_s) elementList;
};
   
SLIST_HEAD(slisthead, slist_threadData_s) listHead;
                                           
pthread_mutex_t exclusiveAccessWriteFile;  // Mutex for exclusive access to write file 

/**
 * Function:         cleanUpFunction
 * Description:      Function in charge of cleaning any dandling data that may be in used by the server 
 * Parameter:        -
 * Pre:              -
 * Post:             The exclusiveAccessWriteFile mutex is deleted.
 *                   The socket (my_socket) is closed).
 *                   The file to write the results is closed.
 * Return:           void
 */
void cleanUpFunction() {
  syslog(LOG_DEBUG, "Closing socket.");
  shutdown(my_socket, SHUT_RDWR);
  close(my_socket);

  syslog(LOG_DEBUG, "Destroying the mutex.");
  pthread_mutex_destroy(&exclusiveAccessWriteFile);
  
  syslog(LOG_DEBUG, "Deleting file %s.", FILE_TO_READWRITE);
  fclose(filePointer);
  remove(fileToWrite);
}

struct t_eventData{
    int myData;
};

void expired(union sigval timer_data){
  struct t_eventData *data = timer_data.sival_ptr;
    
  time_t rawtime;
  struct tm *local_time;
  char   buffer[80];

  if (filePointer != NULL) {
    pthread_mutex_lock(&exclusiveAccessWriteFile);

    time( &rawtime );
    local_time = localtime( &rawtime );
    strftime(buffer, sizeof(buffer),"%F %T:%M%p", local_time);

    syslog(LOG_USER, "Storing timestamp in file: %s\n", buffer);
    fseek(filePointer, 0, SEEK_END);	
    fprintf(filePointer, "timestamp: %s\n", buffer);
    pthread_mutex_unlock(&exclusiveAccessWriteFile);
  }
 
  syslog(LOG_DEBUG, "Timer fired %d times\n", ++data->myData);
}

/**
 * Function:         signal_handler
 * Description:      Function in charge of handling interrupts received by the program
 * Parameter:        signal_number -> Signal raised towards the program
 * Pre:
 * Post:             caught_sigint == true
 *                   caught_sigterm == true
 *                   filePointer is closed and deleted
 * Return:           void
 */
static void signal_handler(int signal_number) {
  sigset_t previousMask;   
  sigset_t currentMask;
  sigprocmask(SIG_SETMASK, NULL, &previousMask);
  
  syslog(LOG_INFO, "Caught signal, exiting.");
  
  sigfillset(&currentMask);
  sigprocmask(SIG_SETMASK, &currentMask, NULL);
  
  if (signal_number == SIGINT) {
    caught_sigint = true;
  } else if (signal_number == SIGTERM) {
    caught_sigterm = true;
  }
  
  syslog(LOG_DEBUG, "Closing socket.");
  close(my_socket);

  syslog(LOG_DEBUG, "Deleting file %s.", FILE_TO_READWRITE);
  fclose(filePointer);
  remove(fileToWrite);
  
  cleanUpFunction();
  
  sigprocmask(SIG_SETMASK, &previousMask, NULL);
}

/**
 * Function:         create_file
 * Description:      Function in charge of opening for writing the file pass as argument. If
 *                   the file exist, the function truncates it.
 * Parameter:        theFileName -> File name to be opened.
 * Pre:              theFileName != NULL
 * Post:             myFilePointer == FILE handler of the file.
 * Return:           The pointer to FILE handler of the open file or NULL if the
 *                   preconditions are not met.
 */
FILE* create_file(const char* theFileName) {
   FILE* myFilePointer = NULL;
   
   if (theFileName != NULL) {
     myFilePointer = fopen(theFileName, "w+");
     if (myFilePointer == NULL) {
       syslog(LOG_ERR, "Not possible to open the file to writedata received: %s\n", gai_strerror(errno));
     }
   }
   
   return myFilePointer;
}

/**
 * Function:         create_socket
 * Description:      Function in charge of opening the socket for receiving incoming data.
 * Parameter:        thePort -> String with the port to open.
 * Pre:              thePort != NULL
 * Post:             The socket has been opened or an interrupt has been received.
 * Return:           The the handler to the opened socket or -1 if an error or an interrupt
 *                   is found or preconditions are not met.
 */
int create_socket(const char* thePort) {
   int                  status;
   struct addrinfo      hints;
   struct addrinfo*     servinfo;  // will point to the results
   int                  my_socketLocal = 0;
   int                  option         = 1;
   
   if (thePort == NULL) {
     return -1;
   }

   memset(&hints, 0, sizeof(hints));   // make sure the struct is empty
   hints.ai_family    = AF_UNSPEC;     // don't care IPv4 or IPv6
   hints.ai_socktype  = SOCK_STREAM;   // TCP stream sockets
   hints.ai_flags     = AI_PASSIVE;    // fill in my IP for me

   if ((status = getaddrinfo(NULL, thePort, &hints, &servinfo)) != 0) {
     fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
     return -1;
   }

   status = -1;
   while (status == -1) {
      if (caught_sigint || caught_sigterm) {
        return -1;
      }

      // make a socket
      my_socketLocal = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
      if (my_socketLocal != -1) {
   
         if (setsockopt(my_socketLocal, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(option)) == -1) {
            perror("setsockopt");
           fprintf(stderr, "setsockopt error: %s\n", gai_strerror(status));
           return -1;
         }

         // bind it to the port we passed in to getaddrinfo();
         status = bind(my_socketLocal, servinfo->ai_addr, servinfo->ai_addrlen);
         if (status == -1) {
           fprintf(stderr, "bind error: %s\n", gai_strerror(errno));
           close(my_socketLocal);
         }
      }
   }
      // free the memory allocated for result
   freeaddrinfo(servinfo);

   return my_socketLocal;
}

/**
 * Function:         readDataFromSocket
 * Description:      Function in charge of reading data from socket.
 * Parameter:        sock_fd -> Socket file descriptor.
 * Pre:              sock_fd >= 0
 * Post:             Data has been read from the socket or NULL if no data is read.
 * Return:           The data received in the socket.
 */
char* readDataFromSocket(int sock_fd){

    if (sock_fd < 0){
        return NULL;
    }

    char* buffer = (char * )malloc(1);
    memset((void *) buffer, '\0', 1);
    int ret_value = -1;
    size_t capacity = 0;
    size_t filled = 0;

    do {
        char byte;
        ret_value = read(sock_fd, &byte, 1);

        if (ret_value == -1){
            break;
        }
        ++filled;
        if (filled >= capacity){
            capacity += filled * 4;
            syslog(LOG_DEBUG, "Expanding buffer to %ld", capacity);
            void* new = realloc((void*)buffer, capacity);

            if (!new){
                return NULL;
            }
            buffer = (char * )new;
        }

        buffer[filled] = '\0';
        buffer[filled - 1] = byte;

        if (byte == '\n'){
            break;
        }

    } while (ret_value != 0);

    return buffer;
}


/**
 * Function:         handleConnection
 * Description:      Function that creates one thread per connection to receive incoming packets.
 * Parameter:        their_addr -> Addres of the client.
 *                   new_socket -> New socket with the connection.
 * Pre:              -
 * Post:             Data has been read from the socket or NULL if no data is read.
 * Return:           -
 */
void* handleConnection(void* connectionDetails) {
                     
  struct sockaddr_in   their_addr;
  int                  new_socket;
  char*                receivedMessage = NULL;
  int                  stringLength    = 0;
  int                  numbytesSent    = 0;
  int                  indexInitBuffer = 0;
    
  their_addr = ((struct slist_threadData_s*) connectionDetails)->their_addr;
  new_socket = ((struct slist_threadData_s*) connectionDetails)->new_socket;

  char *ipv4Address_str = inet_ntoa(((struct sockaddr_in*) &their_addr)->sin_addr);
  syslog(LOG_INFO, "Accepted connection from %s\n", ipv4Address_str);
    
  receivedMessage = readDataFromSocket(new_socket);
  if (receivedMessage == NULL) {
    return NULL;
  }
  if (receivedMessage != NULL) {
    syslog(LOG_DEBUG, "Received number of bytes = %ld", strlen(receivedMessage));

    if (filePointer != NULL) {
      pthread_mutex_lock(&exclusiveAccessWriteFile);
      syslog(LOG_USER, "Storing contents in file: %s\n", receivedMessage);
      fseek(filePointer, 0, SEEK_END);	
      fprintf(filePointer, "%s", receivedMessage);
      pthread_mutex_unlock(&exclusiveAccessWriteFile);
    }

    fseek(filePointer, 0, SEEK_SET);
    while (true) {
      if (!fgets(receivedMessage, sizeof(receivedMessage), filePointer))
        break;
            
      stringLength    = strlen(receivedMessage);

      numbytesSent    = 0;
      indexInitBuffer = 0;
      numbytesSent = send(new_socket, &receivedMessage[indexInitBuffer], stringLength, 0);
      
      while ((stringLength != -1) && (stringLength != numbytesSent)) {
        stringLength    -= numbytesSent;
        indexInitBuffer += numbytesSent;
        numbytesSent = send(new_socket, &receivedMessage[indexInitBuffer], stringLength, 0);
      }
    }
    free(receivedMessage);
  }

  syslog(LOG_INFO, "Closed connection from %s\n", ipv4Address_str);
  close(new_socket);
  
  return NULL;

}

/**
 * Function:         receiveMessages
 * Description:      Function in charge of receiving data and sending it back to the client.
 * Parameter:        SocketFileDescriptor -> Socket file descriptor.
 * Pre:              sock_fd >= 0
 * Post:             Data has been read from the socket and send it back to the client or
 *                   -1 is returned if an error is encountered.
 * Return:           The 0 value is returned if the funcion finishes successfully or -1 
 *                   if an error is found.
 */
int receiveMessages(int SocketFileDescriptor) {
  int                  status          = 0;

  socklen_t            addr_size;
  
  struct slist_threadData_s*
                       connectionDetailsP;
  pthread_t            theThread;
  
  if (SocketFileDescriptor == -1) {
    syslog(LOG_ERR, "Invalid socket file descriptor");
    return -1;
  }
  
  remove(fileToWrite);
  
  filePointer = create_file(fileToWrite);
  if (filePointer == NULL) {
    return -1;
  }
  
  status = listen(my_socket, BACKLOG);
  if (status == -1) {
    fprintf(stderr, "listen error: %s\n", gai_strerror(errno));
    return -1;
  }
  
  while (true) {
    connectionDetailsP = malloc(sizeof(struct slist_threadData_s));
  
    syslog(LOG_DEBUG, "Accepting new connection.");
    addr_size = sizeof(connectionDetailsP->their_addr);
    connectionDetailsP->new_socket = accept(my_socket, (struct sockaddr*) &connectionDetailsP->their_addr, &addr_size);

    if (connectionDetailsP->new_socket == -1) {
      if (!caught_sigint && !caught_sigterm) {
        syslog(LOG_ERR, "socket accept error: %s\n", gai_strerror(errno));
        usleep(1000);
        continue;
      }
      else {
        return -1;
      }
    }
    
    // Calling handleConnection to create one thread per connection
    pthread_create(&theThread, NULL, handleConnection, connectionDetailsP);

    connectionDetailsP->theThreadId     = theThread;
    connectionDetailsP->threadCompleted = false;
    connectionDetailsP->threadDeleted   = false;
       
    // Inserting the thread in the list of threads.
    syslog(LOG_DEBUG, "The thread Id (%lu) has been created.\n", connectionDetailsP->theThreadId);
    SLIST_INSERT_HEAD(&listHead, connectionDetailsP, elementList);
    
    //  Checking if any thread has been terminated / completed
        // Read1.
    SLIST_FOREACH(connectionDetailsP, &listHead, elementList) {
      if ((connectionDetailsP->threadCompleted) && 
          (!connectionDetailsP->threadDeleted)) {
        syslog(LOG_DEBUG, "The thread Id (%lu) has completed its execution)", connectionDetailsP->theThreadId);
        pthread_join(connectionDetailsP->theThreadId, NULL);
      
        // Marking thread as deleted.
        connectionDetailsP->threadDeleted = true;
      }
    }
  }

  fclose(filePointer);
  shutdown(SocketFileDescriptor, SHUT_RDWR);
  
  return 0;
}


int main(int argc, char *argv[]) {
   int                         returnCode   = 0;
   int                         returnValue  = 0;
   bool                        runAsDaemon  = false;
   pid_t                       theChildPid  = 0;
   pid_t                       theSid       = 0;
   
   // Variables used to create a timer to be raised every 10 seconds.
   struct t_eventData         eventData = { .myData = 0 };
   struct sigevent            sev = { 0 };                               // sigevent specifies behaviour on expiration
   struct itimerspec          its = {.it_value.tv_sec     = 10,          // specify start delay and interval
                                     .it_value.tv_nsec    = 0,
                                     .it_interval.tv_sec  = 10,          // it_value and it_interval must not be zero
                                     .it_interval.tv_nsec = 0
                                    };
   timer_t                    timerId                     = 0;
   
   openlog(argv[0], LOG_PID|LOG_CONS, LOG_LOCAL0);
   syslog(LOG_USER, "Syslog has been setup for aesdsocket.log\n");

   SLIST_INIT(&listHead);
   
   if (pthread_mutex_init(&exclusiveAccessWriteFile, NULL) != 0) { 
     syslog(LOG_USER, "Mutex init for file write access has failed\n"); 
     exit(EXIT_FAILURE);
   } 
   
   my_socket = create_socket(PORT);
   if (my_socket == -1) {
     syslog(LOG_ERR, "Error when opening the socket for receiving data");
     exit(EXIT_FAILURE);
   }
   
   if (argc == 2) {
    if (strcmp(argv[1], "-d") == 0) {
      runAsDaemon = true;
      syslog(LOG_USER, "Running in daemon mode\n");
    }
   }
   
   struct sigaction            new_action;
   memset(&new_action, 0, sizeof(struct sigaction));
   new_action.sa_handler = signal_handler;
   
   if (sigaction(SIGTERM, &new_action, NULL) != 0) {
      fprintf(stderr, "Error: %d (%s) registering for SIGTERM", errno, strerror(errno));
      exit(-1);
   }

   if (sigaction(SIGINT, &new_action, NULL) != 0) {
      fprintf(stderr, "Error: %d (%s) registering for SIGINT", errno, strerror(errno));
      exit(-1);
   }

   // The following process is mandatory to get a daemon runnning and detached from the
   // console or terminal
   if (runAsDaemon) {
      syslog(LOG_DEBUG, "Forking");
      
      if ((theChildPid = fork()) < 0) {
        syslog(LOG_ERR, "Fork failed");
        exit(EXIT_FAILURE);
      }
      else if (theChildPid > 0) {
        syslog(LOG_INFO, "Child process id = %d", theChildPid);
        exit(EXIT_SUCCESS); 
      }
      
      // Now seting new sesion
      theSid = setsid();
      if (theSid < 0) {
        syslog(LOG_ERR, "setsid() failed");
        exit(EXIT_FAILURE);
      }
   }
   
   // Creating the timer to be raised every 10 seconds
   sev.sigev_notify          = SIGEV_THREAD;
   sev.sigev_notify_function = &expired;
   sev.sigev_value.sival_ptr = &eventData;
   
       /* create timer */
   returnValue = timer_create(CLOCK_REALTIME, &sev, &timerId);
   if (returnValue != 0){
     syslog(LOG_ERR, "Error when creating the timer (calling timer_create function): %s\n", strerror(errno));
     cleanUpFunction();
     exit(EXIT_FAILURE);
   }

   /* start timer */
   returnValue = timer_settime(timerId, 0, &its, NULL);
   if (returnValue != 0){
     syslog(LOG_ERR, "Error timer_settime: %s\n", strerror(errno));
     cleanUpFunction();
     exit(EXIT_FAILURE);
   }
   syslog(LOG_DEBUG, "Timer to be raised every 10 seconds has been created");
   
   returnValue = receiveMessages(my_socket);
   if (returnValue != 0) {
     syslog(LOG_ERR, "Failure when reading message in the socket");
     remove(FILE_TO_READWRITE);
     exit(EXIT_FAILURE);
   }
   
   pthread_mutex_destroy(&exclusiveAccessWriteFile);
   
   return returnCode;
}

