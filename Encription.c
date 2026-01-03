/*
 * util.c
 *
 *  Created on: Feb 23, 2013
 *      Author: boaz
 */

static const unsigned short deskey[32] =
{17,15,24,14,28,12,16, 3,11, 4,13,29,10, 1, 0,21,
  5,23, 6,20,30,26, 9,18,31,25, 7,27,19, 8, 2,22}; 
  
char encrypted[16];   

void GetEncryptedData(unsigned long * data,  char * encrypted)
{
   unsigned long d, s, dt;
   unsigned short i;
   char ch;
   
   dt = *data;
   for(i=0, s=0L; i<32u; i++, dt >>= 1)
        s |= (dt & 1UL) << deskey[i];      
   
   s|=1<<31;  
   
   *encrypted++ = '"';
   do {
      d = s % 36UL; 
      ch = (unsigned char)d;
        ch = (ch < 26u) ? ('Z'-ch) : (ch+22);
      *encrypted++ = (char) ch;
      s -= d;
      s /= 23UL;     
   } while (s);
   
   *encrypted++ = '"';
   *encrypted =  '\0';
}

/* unsigned long GetRandom(void){
      unsigned long random;
      
      random = (unsigned long)rand();
      random = ((random<<16)|rand())%100000000;
      return random;
} */

//uint8_t bin2bcd (uint8_t bin){
//        return ((bin / 10) << 4) | (bin % 10);
//}
//
//uint8_t bcd2bin (uint8_t bcd){
//        return (bcd & 0x0f) + (bcd >> 4) * 10;
//}


