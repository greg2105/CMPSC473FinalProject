#include "channel.h"

// Creates a new channel with the provided size and returns it to the caller
// A 0 size indicates an unbuffered channel, whereas a positive size indicates a buffered channel
chan_t* channel_create(size_t size)
{   
    if (size == 0) {
        return NULL; // unbuffered channels don't have to be supported
    }

    buffer_t* buffer = buffer_create(size);
    chan_t* channel = (chan_t*) malloc(sizeof(chan_t));
    channel->buffer = buffer;
    channel->semaphore = NULL;
    channel->open = 1;
    pthread_cond_init(&channel->recv, NULL);
    pthread_cond_init(&channel->send, NULL);
    pthread_mutex_init(&channel->mutex, NULL);
    
    return channel;
}

// Writes data to the given channel
// This can be both a blocking call i.e., the function only returns on a successful completion of send (blocking = true), and
// a non-blocking call i.e., the function simply returns if the channel is full (blocking = false)
// In case of the blocking call when the channel is full, the function waits till the channel has space to write the new data
// Returns SUCCESS for successfully writing data to the channel,
// WOULDBLOCK if the channel is full and the data was not added to the buffer (non-blocking calls only),
// CLOSED_ERROR if the channel is closed, and
// OTHER_ERROR on encountering any other generic error of any sort
enum chan_status channel_send(chan_t *channel, void* data, bool blocking)
{
    pthread_mutex_lock(&channel->mutex);
    if(!channel->open){
        pthread_mutex_unlock(&channel->mutex);
        return CLOSED_ERROR;
    }

    //HANDLES THE CASE OF BLOCKING
    if(blocking){
        while(buffer_capacity(channel->buffer) == buffer_current_size(channel->buffer)){
            if(!channel->open){
                pthread_mutex_unlock(&channel->mutex);
                return CLOSED_ERROR;
            }
            pthread_cond_wait(&channel->send, &channel->mutex);
        }

        if (channel->open){
            if(!buffer_add(data, channel->buffer)){
                pthread_mutex_unlock(&channel->mutex);
                return OTHER_ERROR;
            }
        }

        if(channel->semaphore){
            sem_post(channel->semaphore);
        }
    }
    //HANDLES THE CASE OF NON-BLOCKING
    else {
        if(buffer_capacity(channel->buffer) == buffer_current_size(channel->buffer)){
            pthread_mutex_unlock(&channel->mutex);
            return WOULDBLOCK;
        }

        if(channel->open){
            if(!buffer_add(data, channel->buffer)){
                pthread_mutex_unlock(&channel->mutex);
                return OTHER_ERROR;
            }
        }

        if(channel->semaphore){
            sem_post(channel->semaphore);
        }
    }
    pthread_cond_signal(&channel->recv);
    pthread_mutex_unlock(&channel->mutex);
    return SUCCESS;
}

// Reads data from the given channel and stores it in the function’s input parameter, data (Note that it is a double pointer).
// This can be both a blocking call i.e., the function only returns on a successful completion of receive (blocking = true), and
// a non-blocking call i.e., the function simply returns if the channel is empty (blocking = false)
// In case of the blocking call when the channel is empty, the function waits till the channel has some data to read
// Returns SUCCESS for successful retrieval of data,
// WOULDBLOCK if the channel is empty and nothing was stored in data (non-blocking calls only),
// CLOSED_ERROR if the channel is closed, and
// OTHER_ERROR on encountering any other generic error of any sort
enum chan_status channel_receive(chan_t* channel, void** data, bool blocking)
{
    pthread_mutex_lock(&channel->mutex);
    if(!channel->open){
        pthread_mutex_unlock(&channel->mutex);
        return CLOSED_ERROR;
    }

    //HANDLES THE CASE OF BLOCKING
    if(blocking){
        while(!buffer_current_size(channel->buffer)){
            if(!channel->open){
                pthread_mutex_unlock(&channel->mutex);
                return CLOSED_ERROR;
            }
            pthread_cond_wait(&channel->recv, &channel->mutex);
        }
        if (channel->open){
            *data = buffer_remove(channel->buffer);
            if(!data){
                pthread_mutex_unlock(&channel->mutex);
                return OTHER_ERROR;
            }
        }
        if(channel->semaphore){
            sem_post(channel->semaphore);
        }
    }

    //HANDLES THE CASE OF NON BLOCKING
    else{
        if(!buffer_current_size(channel->buffer)){
            pthread_mutex_unlock(&channel->mutex);
            return WOULDBLOCK;
        }
        if(channel->open){
            *data = buffer_remove(channel->buffer);
            if(!data){
                pthread_mutex_unlock(&channel->mutex);
                return OTHER_ERROR;
            }
        }
        if(channel->semaphore){
            sem_post(channel->semaphore);
        }
    }

    pthread_cond_signal(&channel->send);
    pthread_mutex_unlock(&channel->mutex);
    return SUCCESS;
    
}


// Closes the channel and informs all the blocking send/receive/select calls to return with CLOSED_ERROR
// Once the channel is closed, send/receive/select operations will cease to function and just return CLOSED_ERROR
// Returns SUCCESS if close is successful,
// CLOSED_ERROR if the channel is already closed, and
// OTHER_ERROR in any other error case
enum chan_status channel_close(chan_t* channel)
{
   pthread_mutex_lock(&channel->mutex);

    if(!channel->open){
        pthread_mutex_unlock(&channel->mutex);
        return CLOSED_ERROR;
    }
    else{
        channel->open = false;
        pthread_cond_broadcast(&channel->send);
        pthread_cond_broadcast(&channel->recv);

        if(channel->semaphore){
            sem_post(channel->semaphore);
        }
        pthread_mutex_unlock(&channel->mutex);
        return SUCCESS;
    }
    pthread_mutex_unlock(&channel->mutex);
    return OTHER_ERROR;
}

// Frees all the memory allocated to the channel
// The caller is responsible for calling channel_close and waiting for all threads to finish their tasks before calling channel_destroy
// Returns SUCCESS if destroy is successful,
// DESTROY_ERROR if channel_destroy is called on an open channel, and
// OTHER_ERROR in any other error case
enum chan_status channel_destroy(chan_t* channel)
{
    if(channel->open){
        return DESTROY_ERROR;
    }

    else{
        buffer_free(channel->buffer);
        pthread_cond_destroy(&channel->recv);
        pthread_cond_destroy(&channel->send);
        pthread_mutex_destroy(&channel->mutex);
        free(channel);
        return SUCCESS;
    }

    return OTHER_ERROR;
}

// Takes an array of channels (channel_list) of type select_t and the array length (channel_count) as inputs
// This API iterates over the provided list and finds the set of possible channels which can be used to invoke the required operation (send or receive) specified in select_t
// If multiple options are available, it selects the first option and performs its corresponding action
// If no channel is available, the call is blocked and waits till it finds a channel which supports its required operation
// Once an operation has been successfully performed, select should set selected_index to the index of the channel that performed the operation and then return SUCCESS
// In the event that a channel is closed or encounters any error, the error should be propagated and returned through select
// Additionally, selected_index is set to the index of the channel that generated the error
enum chan_status channel_select(size_t channel_count, select_t* channel_list, size_t* selected_index)
{
    sem_t *semaphore = (sem_t*)malloc(sizeof(sem_t));
    sem_init(semaphore, 1, 0);

    for(int i = 0; i < channel_count; i++){
        pthread_mutex_lock(&(channel_list[i].channel->mutex));
        channel_list[i].channel->semaphore = semaphore;
        pthread_mutex_unlock(&(channel_list[i].channel->mutex));
    }

    while(true){ //check if valid
        for(int i = 0; i < channel_count; i++){
            if(channel_list[i].is_send){
                pthread_mutex_lock(&(channel_list[i].channel->mutex));
                if(buffer_capacity(channel_list[i].channel->buffer) > buffer_current_size(channel_list[i].channel->buffer)){
                    *selected_index = (size_t)i;
                    pthread_mutex_unlock(&(channel_list[i].channel->mutex));
                    return channel_send(channel_list[i].channel, &channel_list[i].data, true);
                }
            }
            else{
                pthread_mutex_lock(&(channel_list[i].channel->mutex));
                if(buffer_current_size(channel_list[i].channel->buffer)){
                    *selected_index = (size_t)i;
                    pthread_mutex_unlock(&(channel_list[i].channel->mutex));
                    return channel_receive(channel_list[i].channel, &channel_list[i].data, true);
                }
            }
        }
        sem_wait(semaphore);
    }

    return OTHER_ERROR;
}