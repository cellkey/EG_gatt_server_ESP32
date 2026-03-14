#ifndef ENCRYPTION_H
#define ENCRYPTION_H

void GetEncryptedData(unsigned long *data, char *encrypted);

// External variables that main file will provide
extern char encrypted_data[10];
extern unsigned long rnd ;

#endif