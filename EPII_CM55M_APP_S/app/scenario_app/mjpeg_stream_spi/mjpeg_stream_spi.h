#ifndef MJPEG_STREAM_SPI_H_
#define MJPEG_STREAM_SPI_H_

#define APP_BLOCK_FUNC() do{ \
	__asm volatile("b    .");\
	}while(0)

void app_main(void);

#endif /* MJPEG_STREAM_SPI_H_ */
