#include <string.h>
#include <stdio.h>
#include <sodium.h>

static char *fileMagic = "RCLONE\x00\x00";
#define fileMagicSize 8
#define fileNonceSize 24
#define fileHeaderSize (fileMagicSize + fileNonceSize)
#define blockHeaderSize crypto_secretbox_MACBYTES
#define blockDataSize (64 * 1024)
#define blockSize (blockHeaderSize + blockDataSize)
static uint8_t defaultSalt[] = {
	0xA8,0x0D,0xF4,0x3A,0x8F,0xBD,0x03,0x08,
	0xA7,0xCA,0xB8,0x3E,0x58,0x1F,0x86,0xB1};

static uint8_t plainblock[blockDataSize];
static uint8_t chiperblock[blockSize];

// uint8_t dataKey[32]
int makeKey(const uint8_t *password, size_t passwordlen,
		const uint8_t *salt, size_t saltlen,
		uint8_t *dataKey)
{
	int keySize = 32;
	if (saltlen == 0) {
		return crypto_pwhash_scryptsalsa208sha256_ll(
				password, passwordlen,
				defaultSalt, 16,
				16384, 8, 1,
				dataKey, keySize);
	}
	return crypto_pwhash_scryptsalsa208sha256_ll(
			password, passwordlen,
			salt, saltlen,
			16384, 8, 1,
			dataKey, keySize);
}

int encrypt_file(char *inputfile, char *outputfile, const uint8_t *dataKey)
{
	FILE *infile = fopen(inputfile, "rb");
	if(!infile) {
		printf("failed to open input file: %s\n", inputfile);
		return 1;
	}
	FILE *outfile = fopen(outputfile, "wb");
	if(!outfile) {
		printf("failed to open output file: %s\n", outputfile);
		fclose(infile);
		return 1;
	}

	// generate file nonce
	uint8_t nonce[fileNonceSize];
	randombytes_buf(nonce, sizeof(nonce));

	printf("file nonce:\n");
	for (int i = 0; i < fileNonceSize; i++) {
		printf("%02x ", nonce[i]);
	}
	printf("\n");

	// Cipher fileheader
	// File magic bytes
	if(fwrite(fileMagic, 1, fileMagicSize, outfile) != fileMagicSize) {
		printf("failed to write(magic) output file: %s\n", outputfile);
		fclose(infile);
		fclose(outfile);
		return 1;
	}
	// File nonce bytes
	if(fwrite(nonce, 1, fileNonceSize, outfile) != fileNonceSize) {
		printf("failed to write(nonce) output file: %s\n", outputfile);
		fclose(infile);
		fclose(outfile);
		return 1;
	}

	int i = 0;
	size_t mlen = blockDataSize;
	while(mlen == blockDataSize) {
		printf("block %d\n",i);

		// load plain block
		mlen = fread(plainblock, 1, blockDataSize, infile);
		if(mlen < 0) {
			printf("failed to read input file: %s\n", inputfile);
			fclose(infile);
			fclose(outfile);
			return 1;
		}
		if(mlen == 0)
			break;

		// encrypt block
		if(crypto_secretbox_easy(chiperblock, plainblock, mlen, nonce, dataKey) != 0) {
			printf("failed to seal\n");
			fclose(infile);
			fclose(outfile);
			return 1;	
		}

		// if shorter block, clip length
		size_t chiperlen = blockHeaderSize + mlen;

		// write encrypted file
		if(fwrite(chiperblock, 1, chiperlen, outfile) != chiperlen) {
			printf("failed to write(%d body) output file: %s\n", i, outputfile);
			fclose(infile);
			fclose(outfile);
			return 1;
		}

		// increment nonce
		sodium_increment(nonce, sizeof(nonce));
		i++;
	}

	fclose(infile);
	fclose(outfile);
	return 0;
}

int decrypt_file(char *inputfile, char *outputfile, const uint8_t *dataKey)
{
	FILE *infile = fopen(inputfile, "rb");
	if(!infile) {
		printf("failed to open input file: %s\n", inputfile);
		return 1;
	}
	FILE *outfile = fopen(outputfile, "wb");
	if(!outfile) {
		printf("failed to open output file: %s\n", outputfile);
		fclose(infile);
		return 1;
	}

	// Header check
	uint8_t magic[fileMagicSize];
	if(fread(magic, 1, fileMagicSize, infile) < fileMagicSize) {
		printf("failed to read(magic) input file: %s\n", inputfile);
		fclose(infile);
		fclose(outfile);
		return 1;
	}
	if(memcmp(magic, fileMagic, fileMagicSize) != 0) {
		printf("Header magic not found in input file: %s\n", inputfile);
		fclose(infile);
		fclose(outfile);
		return 1;
	}

	// restore nonce
	uint8_t nonce[fileNonceSize];
	if(fread(nonce, 1, fileNonceSize, infile) < fileNonceSize) {
		printf("failed to read(nonce) input file: %s\n", inputfile);
		fclose(infile);
		fclose(outfile);
		return 1;
	}

	printf("file nonce:\n");
	for (int i = 0; i < fileNonceSize; i++) {
		printf("%02x ", nonce[i]);
	}
	printf("\n");

	int i = 0;
	size_t clen = blockSize;
	while(clen == blockSize) {
		printf("block %d\n",i);

		// load chiper block
		clen = fread(chiperblock, 1, blockSize, infile);
		if(clen < 0) {
			printf("failed to read input file: %s\n", inputfile);
			fclose(infile);
			fclose(outfile);
			return 1;
		}
		if(clen == 0)
			break;
		if(clen <= blockHeaderSize) {
			printf("input file is broken(block %d, size %d): %s\n", i, (int)clen, inputfile);
			fclose(infile);
			fclose(outfile);
			return 1;
		}	

		// decrypt block
		if(crypto_secretbox_open_easy(plainblock, chiperblock, clen, nonce, dataKey) != 0) {
			printf("failed to open\n");
			fclose(infile);
			fclose(outfile);
			return 1;	
		}

		// if shorter block, clip length
		size_t plainlen = clen - blockHeaderSize;
		
		// write decrypted file
		if(fwrite(plainblock, 1, plainlen, outfile) != plainlen) {
			printf("failed to write(%d body) output file: %s\n", i, outputfile);
			fclose(infile);
			fclose(outfile);
			return 1;
		}
		
		// increment nonce
		sodium_increment(nonce, sizeof(nonce));
		i++;
	}

	fclose(infile);
	fclose(outfile);
	return 0;
}

int main(int argc, char *argv[])
{
	if (sodium_init() == -1) {
		printf("failed to init sodium library\n");
		return 1;
	}
	if (argc < 6) {
		printf("usage: %s [c | d] (target) (output) (password) (salt)\n", argv[0]);
		return 1;
	}
	int mode = 0;
	if (strlen(argv[1]) >= 1) {
		if (argv[1][0] == 'c') {
			printf("encryption mode\n");
			mode = 1;
		}
		if (argv[1][0] == 'd') {
			printf("decryption mode\n");
			mode = 2;
		}
	}
	if (mode == 0) {
		printf("select encrypt(c) or decrypt(d)\n");
		printf("usage: %s [c | d] (target) (output) (password) (salt)\n", argv[0]);
		return 1;
	}

	char *passwd = argv[4];
	char *salt = argv[5];
	int passwdlen = strlen(passwd);
	int saltlen = strlen(salt);
	
	printf("passwd: %s\nsalt: %s\n", passwd, salt);

	uint8_t dataKey[32];
	if(makeKey((uint8_t *)passwd, passwdlen, (uint8_t *)salt, saltlen, dataKey) != 0) {
		printf("failed to makeKey()\n");
		return 1;
	}
	
	if (mode == 1)
		return encrypt_file(argv[2], argv[3], dataKey);
	if (mode == 2)
		return decrypt_file(argv[2], argv[3], dataKey);
}
