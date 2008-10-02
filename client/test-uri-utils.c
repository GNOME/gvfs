
#include <string.h>

#include "gvfsuriutils.h"


typedef struct {
	const char *uri;
	const char *expected_host;
	guint expected_port;
} TestURIs;

static TestURIs uris[] = {
	{ "https://[2001:0db8:85a3:08d3:1319:8a2e:0370:7344]:443/", "[2001:0db8:85a3:08d3:1319:8a2e:0370:7344]", 443 },
	{ "http://test:443/", "test", 443 },
	{ "http://test/", "test", -1 },
	{ "obex://[00:FF:FF:FF:FF:FF]/MMC/foo.jpg", "[00:FF:FF:FF:FF:FF]", -1 },
	{ "obex://[00:FF:FF:FF:FF:FF]/C:", "[00:FF:FF:FF:FF:FF]", -1 },
	{ "http://windows-host:8080/C:/", "windows-host", 8080 },
	{ "smb://user:password@192.192.192.192/foobar", "192.192.192.192", -1 },
	{ "https://d134w4tst3t.s3.amazonaws.com/a?Signature=6VJ9%2BAdPVZ4Z7NnPShRvtDsLofc%3D&Expires=1249330377&AWSAccessKeyId=0EYZF4DV8A7WM0H73602", "d134w4tst3t.s3.amazonaws.com", -1 },
};

int main (int argc, char **argv)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (uris); i++) {
		GDecodedUri *decoded;
		char *encoded;

		decoded = g_vfs_decode_uri (uris[i].uri);
		if (decoded == NULL) {
			g_warning ("Failed to parse \"%s\"", uris[i].uri);
			return 1;
		}
		if (decoded->host == NULL || strcmp (decoded->host, uris[i].expected_host) != 0) {
			g_warning ("Wrong host for \"%s\" (got '%s', expected '%s')", uris[i].uri, decoded->host, uris[i].expected_host);
			g_vfs_decoded_uri_free (decoded);
			return 1;
		}
		if (decoded->port != uris[i].expected_port) {
			g_warning ("Wrong port for \"%s\"", uris[i].uri);
			g_vfs_decoded_uri_free (decoded);
			return 1;
		}
		encoded = g_vfs_encode_uri (decoded, TRUE);
		if (encoded == NULL || strcmp (encoded, uris[i].uri) != 0) {
			g_warning ("Failed to re-encode \"%s\" from '%s'", uris[i].uri, encoded);
			g_vfs_decoded_uri_free (decoded);
			g_free (encoded);
			return 1;
		}
		g_free (encoded);
		g_vfs_decoded_uri_free (decoded);
	}

	return 0;
}

