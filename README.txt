1. Code from Beej's tutorial was used to generate most part of our main() function.

2. When retrieving client commands, the number of bytes retrieved is alway 2 more 
   than the actual number of characters in the command. (e.g., if the user command
   is "USER cs317", which contains 10 characters. The number of bytes retrieved
   would be 12. To adjust for this, in the split_buf() function, the code 
   <buf[numbytes-2] = '\0';> is put in to fix the length of the string.
 
