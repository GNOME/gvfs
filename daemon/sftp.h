/* From draft-ietf-secsh-filexfer-13.txt */

/* version */
#define	SSH_FILEXFER_VERSION		3

/* client to server */
#define SSH_FXP_INIT			1
#define SSH_FXP_OPEN			3
#define SSH_FXP_CLOSE			4
#define SSH_FXP_READ			5
#define SSH_FXP_WRITE			6
#define SSH_FXP_LSTAT			7
#define SSH_FXP_STAT_VERSION_0		7
#define SSH_FXP_FSTAT			8
#define SSH_FXP_SETSTAT			9
#define SSH_FXP_FSETSTAT		10
#define SSH_FXP_OPENDIR			11
#define SSH_FXP_READDIR			12
#define SSH_FXP_REMOVE			13
#define SSH_FXP_MKDIR			14
#define SSH_FXP_RMDIR			15
#define SSH_FXP_REALPATH		16
#define SSH_FXP_STAT			17
#define SSH_FXP_RENAME			18
#define SSH_FXP_READLINK		19
#define SSH_FXP_SYMLINK			20

/* server to client */
#define SSH_FXP_VERSION			2
#define SSH_FXP_STATUS			101
#define SSH_FXP_HANDLE			102
#define SSH_FXP_DATA			103
#define SSH_FXP_NAME			104
#define SSH_FXP_ATTRS			105

#define SSH_FXP_EXTENDED		200
#define SSH_FXP_EXTENDED_REPLY		201

/* attributes */
#define SSH_FILEXFER_ATTR_SIZE		0x00000001
#define SSH_FILEXFER_ATTR_UIDGID	0x00000002
#define SSH_FILEXFER_ATTR_PERMISSIONS	0x00000004
#define SSH_FILEXFER_ATTR_ACMODTIME	0x00000008
#define SSH_FILEXFER_ATTR_EXTENDED	0x80000000

/* portable open modes */
#define SSH_FXF_READ			0x00000001
#define SSH_FXF_WRITE			0x00000002
#define SSH_FXF_APPEND			0x00000004
#define SSH_FXF_CREAT			0x00000008
#define SSH_FXF_TRUNC			0x00000010
#define SSH_FXF_EXCL			0x00000020

/* status messages */
#define SSH_FX_OK			0
#define SSH_FX_EOF			1
#define SSH_FX_NO_SUCH_FILE		2
#define SSH_FX_PERMISSION_DENIED	3
#define SSH_FX_FAILURE			4
#define SSH_FX_BAD_MESSAGE		5
#define SSH_FX_NO_CONNECTION		6
#define SSH_FX_CONNECTION_LOST		7
#define SSH_FX_OP_UNSUPPORTED		8
#define SSH_FX_MAX			8

