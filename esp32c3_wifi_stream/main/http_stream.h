#pragma once
void http_stream_start(void);
/* Ask the running /stream task (if any) to finish and release its socket. */
void http_stream_drop_client(void);
int http_stream_clients(void);
