#include <vitasdkkern.h>
#include <taihen.h>
#include <psp2kern/io/fcntl.h>
#include "log.h"

static unsigned int log_buf_ptr = 0;
static char log_buf[16 * 1024];

void log_reset(){
	/*
	 * Ouverture en ajout et non en troncature : l'amont vidait le fichier a
	 * chaque demarrage, ce qui faisait perdre la trace du test precedent des
	 * que la console redemarrait — exactement ce qu'on cherche a observer ici.
	 */
	ksceIoMkdir(LOG_PATH, 6);

	SceUID fd = ksceIoOpen(LOG_FILE,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 6);
	if (fd < 0)
		return;

	static const char sep[] = "\n===== demarrage =====\n";
	ksceIoWrite(fd, sep, sizeof(sep) - 1);
	ksceIoClose(fd);

	memset(log_buf, 0, sizeof(log_buf));
}

void log_write(const char *buffer, size_t length){
	if ((log_buf_ptr + length) >= sizeof(log_buf))
		log_flush();

	memcpy(log_buf + log_buf_ptr, buffer, length);

	log_buf_ptr = log_buf_ptr + length;
}

void log_flush(){
	ksceIoMkdir(LOG_PATH, 6);

	SceUID fd = ksceIoOpen(LOG_FILE,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 6);
	if (fd < 0)
		return;

	ksceIoWrite(fd, log_buf, strlen(log_buf));
	ksceIoClose(fd);
	memset(log_buf, 0, sizeof(log_buf));
	log_buf_ptr = 0;
}