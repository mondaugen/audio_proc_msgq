/* audio_proc_jack.c

   Receive audio data using JACK library.
   Send audio data to a different process via message queue.
   Receive data from a different process via a message queue.
   Send these audio data using JACK library.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include "heap.h"
#include "fastcache.h"

#ifdef __MINGW32__
#include <pthread.h>
#endif

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#endif

#ifndef MAX
#define MAX(a,b) ( (a) < (b) ? (b) : (a) )
#endif

#define MSGQ_AUDIOMSG_TYPE 1
#define AUDIOMSGBUFSIZE (4096*sizeof(jack_default_audio_sample_t))
#define CACHE_NOBJS 1024

static int debug = 0;

static jack_port_t* port;
static jack_port_t* output_port;
static jack_ringbuffer_t *rb_tosubproc = NULL;
static jack_ringbuffer_t *rb_fromsubproc = NULL;
/* mutex for incoming audio events / outgoing messages */
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;
/* mutex for outgoing audio events / incoming messages */
static pthread_mutex_t subproc_outrb_lock = PTHREAD_MUTEX_INITIALIZER;

static int keeprunning = 1;
static uint64_t monotonic_cnt = 0;
Heap *outevheap;
static int passthrough = 0;
jack_nframes_t audio_buf_size;

#define RBSIZE (AUDIOMSGBUFSIZE * 16)

typedef struct {
    /* AUDIO message data */
	char  buffer[AUDIOMSGBUFSIZE];
    /* number of data in message */
	uint32_t size;
    /* time since application started, also used by scheduler to determine when events should be output i.e., the heap sorts by this value so that the event that should happen soonest is always at the top. */
	uint64_t tme_mon;
} audiomsg;

static key_t in_key = 420;
static key_t out_key = 421;

static int msgqid_out;
static int msgqid_in;

struct __attribute__ ((__packed__)) msgq_audiomsg {
    long mtype;
    audiomsg msg;
};
typedef struct msgq_audiomsg msgq_audiomsg;

static void
push_to_output_msgq (audiomsg* event)
{
    msgq_audiomsg tosend;
    tosend.mtype = 1;
    tosend.msg = *event;
    if (msgsnd(msgqid_out,&tosend,sizeof(tosend),0) == -1) {
        perror("msgsnd");
    }
}

int
process (jack_nframes_t frames, void* arg)
{
    /* The count at the beginning of the frame, we need this to calculate the
       offsets into the frame of the outgoing AUDIO messages. */
    uint64_t monotonic_cnt_beg_frame = monotonic_cnt;
	void *buffer, *audiooutbuf;
	jack_nframes_t N;
	jack_nframes_t i;

	buffer = jack_port_get_buffer (port, frames);
    audiooutbuf = jack_port_get_buffer(output_port, frames);
	jack_audio_clear_buffer(audiooutbuf);
	assert (buffer);

    if (passthrough) {
        if (debug) { fprintf(stderr,"passing through\n"); }
        memcpy ( audiooutbuf, buffer, frames * sizeof ( jack_default_audio_sample_t ) );
        return 0;
    }
    jack_ringbuffer_write(rb_tosubproc, buffer, frames * sizeof(jack_default_audio_sample_t));

	if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
		pthread_cond_signal (&data_ready);
		pthread_mutex_unlock (&msg_thread_lock);
	}

    if (pthread_mutex_trylock (&subproc_outrb_lock) == 0) {
        size_t rbnread = 0;
        rbnread = jack_ringbuffer_read(rb_fromsubproc,audiooutbuf,sizeof(jack_default_audio_sample_t)*frames);
        if (rbnread != (sizeof(jack_default_audio_sample_t)*frames)) {
            fprintf(stderr,"Output underrun, requested %zu samples, got %zu.\n",frames,
                    rbnread/sizeof(jack_default_audio_sample_t));
        }
        pthread_mutex_unlock(&subproc_outrb_lock);
    }

	return 0;
}

/* Thread that manages sending messages via Message Queues to the other application */

typedef struct {
    pthread_mutex_t *msg_thread_lock;
    int *keeprunning;
    jack_ringbuffer_t *rb_tosubproc;
    pthread_cond_t *data_ready;
} outthread_data;

static void *
output_thread(void *aux)
{
    outthread_data *thread_data = aux;
    pthread_mutex_lock (thread_data->msg_thread_lock);

    if (debug) { fprintf(stderr,"output thread running\n"); }
    while (*thread_data->keeprunning) {
        while ((jack_ringbuffer_read_space (thread_data->rb_tosubproc)/sizeof(jack_default_audio_sample_t))
                >= audio_buf_size) {
            audiomsg m;
            jack_ringbuffer_read(thread_data->rb_tosubproc, (char*) &m.buffer, 
                    audio_buf_size*sizeof(jack_default_audio_sample_t));
            m.size = audio_buf_size;

            push_to_output_msgq(&m);
            //fprintf(stderr,"message sent\n");
        }
        fflush (stdout);
        pthread_cond_wait (thread_data->data_ready, thread_data->msg_thread_lock);
    }
    pthread_mutex_unlock (thread_data->msg_thread_lock);
    if (debug) { fprintf(stderr,"output thread stopping\n"); }
    return thread_data;
}

/* Thread that manages receiving messages from other application */

typedef struct {
    int *keeprunning;
    int msgqid_in;
    pthread_mutex_t *subproc_outrb_lock;
    jack_ringbuffer_t *rb_fromsubproc;
} inthread_data;

static void *
input_thread(void *aux)
{
    inthread_data *thread_data = aux;
    msgq_audiomsg just_recvd;
    if (debug) { fprintf(stderr,"input thread running\n"); }
    while (*thread_data->keeprunning) {
        /* msgrcv waits for messages on Message Queue */
        if (msgrcv(thread_data->msgqid_in, &just_recvd,
                    sizeof(msgq_audiomsg), MSGQ_AUDIOMSG_TYPE, 0) < 0) {
            perror("msgrcv");
            *thread_data->keeprunning = 0;
            break;
        }
        //fprintf(stderr,"message received\n");
        /* once one is obtained, push to rb_fromsubproc */
        pthread_mutex_lock(thread_data->subproc_outrb_lock);
        size_t nwritten = 0;
        nwritten = jack_ringbuffer_write(thread_data->rb_fromsubproc,
                just_recvd.msg.buffer,
                just_recvd.msg.size*sizeof(jack_default_audio_sample_t));
        if (nwritten != (just_recvd.msg.size*sizeof(jack_default_audio_sample_t))) {
            fprintf(stderr,"Input overrun, tried to write %zu samples, only wrote %zu.\n",
                    just_recvd.msg.size,nwritten/sizeof(jack_default_audio_sample_t));
        } else if (debug){
            fprintf(stderr,"Pushed message to heap.\n");
        }
        pthread_mutex_unlock(thread_data->subproc_outrb_lock);
    }
    if (debug) { fprintf(stderr,"input thread stopping\n"); }
    return thread_data;
}

static void
stopsig(int sn)
{
    keeprunning = 0;
}

static int
audio_bufsize_callback(jack_nframes_t nframes, void *arg)
{
    if ((nframes*sizeof(jack_default_audio_sample_t)) > AUDIOMSGBUFSIZE) {
        fprintf(stderr,"Requested audio buffer size greater than msgq item size!\n");
        return -1;
    }
    *((*jack_nframes_t)arg) = nframes;
    return 0;
}

/* TODO: Make better exit on error that cleans up. */
int
main (int argc, char* argv[])
{
#ifdef DEBUG
    debug = 1;
#else
    debug = 0;
#endif
	jack_client_t* client;
	char const default_name[] = "audio_proc_jack";
	char const * client_name;
	int r;

	int cn = 1;

	if (argc > 1) {
		if (!strcmp (argv[1], "-p")) {
            passthrough = 1;
            cn = 2; 
        }
    }

	if (argc > cn) {
		client_name = argv[cn];
	} else {
		client_name = default_name;
	}

	client = jack_client_open (client_name, JackNullOption, NULL);
	if (client == NULL) {
		fprintf (stderr, "Could not create JACK client.\n");
		exit (EXIT_FAILURE);
	}

	rb_tosubproc = jack_ringbuffer_create (RBSIZE);
    rb_fromsubproc = jack_ringbuffer_create (RBSIZE);
    jack_set_buffer_size_callback(client,
            audio_bufsize_callback,
            &audio_buf_size);
	jack_set_process_callback (client, process, 0);

	port = jack_port_register (client, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	output_port = jack_port_register (client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	if (port == NULL) {
		fprintf (stderr, "Could not register port.\n");
		exit (EXIT_FAILURE);
	}

    msgqid_out = msgget(out_key,0666|IPC_CREAT);
    if (msgqid_out == -1) {
        perror("msgget out");
    }

    msgqid_in = msgget(in_key,0666|IPC_CREAT);
    if (msgqid_in == -1) {
        perror("msgget in");
    }

    if (!outevheap) {
        fprintf(stderr, "Could not allocate heap\n");
        exit (EXIT_FAILURE);
    }

    outthread_data ot_data = {
        .msg_thread_lock = &msg_thread_lock,
        .keeprunning = &keeprunning,
        .rb_tosubproc = rb_tosubproc,
        .data_ready = &data_ready,
    };

    inthread_data it_data = {
        .keeprunning = &keeprunning,
        .msgqid_in = msgqid_in,
        .subproc_outrb_lock = &subproc_outrb_lock,
        .rb_fromsubproc = rb_fromsubproc,
    };

	r = jack_activate (client);
	if (r != 0) {
		fprintf (stderr, "Could not activate client.\n");
		exit (EXIT_FAILURE);
	}

    pthread_t audio_to_msgq;
    pthread_t msgq_to_audio;
    
    signal(SIGINT,stopsig);

    if (pthread_create(&audio_to_msgq,NULL,output_thread,&ot_data)) {
        perror("pthread create audio_to_msgq");
        exit (EXIT_FAILURE);
    }

    if (pthread_create(&msgq_to_audio,NULL,input_thread,&it_data)) {
        perror("pthread create msgq_to_audio");
        exit (EXIT_FAILURE);
    }
    
    pthread_join(audio_to_msgq,NULL);
    pthread_join(msgq_to_audio,NULL);

	jack_deactivate (client);
	jack_client_close (client);
	jack_ringbuffer_free (rb_tosubproc);

	return 0;
}
