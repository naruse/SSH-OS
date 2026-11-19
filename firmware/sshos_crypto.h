/* See sshos_crypto.c. */
#ifndef SSHOS_CRYPTO_H
#define SSHOS_CRYPTO_H

#define SSHOS_CRYPTO_CMD        0x1D    /* the SCSI command */
#define SSHOS_HELLO             0x00    /* its requests, in the second byte */
#define SSHOS_X25519            0x01
#define SSHOS_PUBLIC            0x02
#define SSHOS_SIGN              0x03
#define SSHOS_VERIFY            0x04
#define SSHOS_RESULT            0x10
#define SSHOS_CRYPTO_VERSION    1
#define SSHOS_CRYPTO_MAX        1024    /* the most one request may write */

void sshos_crypto_job(unsigned char request, const unsigned char *in, unsigned int len);
unsigned int sshos_crypto_result(unsigned char *out, unsigned int max);
unsigned int sshos_crypto_hello(unsigned char *out);

/* SSH OS's own arithmetic (kernel/x25519.c, ed25519.c) */
#ifndef SSHOS_KERNEL
void x25519(unsigned char *out32, const unsigned char *scalar32, const unsigned char *point32);
void ed25519_public(unsigned char *pub32, const unsigned char *seed32);
void ed25519_sign(unsigned char *sig64, const unsigned char *msg, unsigned int len,
                  const unsigned char *seed32, const unsigned char *pub32);
int  ed25519_verify(const unsigned char *sig64, const unsigned char *msg, unsigned int len,
                    const unsigned char *pub32);
#endif

#endif
