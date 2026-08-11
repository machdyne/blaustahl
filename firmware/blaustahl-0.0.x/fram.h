#include <stdbool.h>

void fram_init(void);
void fram_read(char *buf, int addr, int len);
void fram_write_enable(void);
void fram_write(int addr, unsigned char d);
bool fram_valid_id(void);
unsigned char spi_xfer(unsigned char d);
