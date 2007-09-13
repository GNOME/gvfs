/* From draft-ietf-secsh-filexfer-13.txt */

#define	SSH2_FILEXFER_VERSION		6

#define	SSH_FXP_INIT                1
#define	SSH_FXP_VERSION             2
#define	SSH_FXP_OPEN                3
#define	SSH_FXP_CLOSE               4
#define	SSH_FXP_READ                5
#define	SSH_FXP_WRITE               6
#define	SSH_FXP_LSTAT               7
#define	SSH_FXP_FSTAT               8
#define	SSH_FXP_SETSTAT             9
#define	SSH_FXP_FSETSTAT           10
#define	SSH_FXP_OPENDIR            11
#define	SSH_FXP_READDIR            12
#define	SSH_FXP_REMOVE             13
#define	SSH_FXP_MKDIR              14
#define	SSH_FXP_RMDIR              15
#define	SSH_FXP_REALPATH           16
#define	SSH_FXP_STAT               17
#define	SSH_FXP_RENAME             18
#define	SSH_FXP_READLINK           19
#define	SSH_FXP_LINK               21
#define	SSH_FXP_BLOCK              22
#define	SSH_FXP_UNBLOCK            23

#define	SSH_FXP_STATUS            101
#define	SSH_FXP_HANDLE            102
#define	SSH_FXP_DATA              103
#define	SSH_FXP_NAME              104
#define	SSH_FXP_ATTRS             105

#define	SSH_FXP_EXTENDED          200
#define	SSH_FXP_EXTENDED_REPLY    201


/* Valid attribute flags */

#define SSH_FILEXFER_ATTR_SIZE              0x00000001
#define SSH_FILEXFER_ATTR_PERMISSIONS       0x00000004
#define SSH_FILEXFER_ATTR_ACCESSTIME        0x00000008
#define SSH_FILEXFER_ATTR_CREATETIME        0x00000010
#define SSH_FILEXFER_ATTR_MODIFYTIME        0x00000020
#define SSH_FILEXFER_ATTR_ACL               0x00000040
#define SSH_FILEXFER_ATTR_OWNERGROUP        0x00000080
#define SSH_FILEXFER_ATTR_SUBSECOND_TIMES   0x00000100
#define SSH_FILEXFER_ATTR_BITS              0x00000200
#define SSH_FILEXFER_ATTR_ALLOCATION_SIZE   0x00000400
#define SSH_FILEXFER_ATTR_TEXT_HINT         0x00000800
#define SSH_FILEXFER_ATTR_MIME_TYPE         0x00001000
#define SSH_FILEXFER_ATTR_LINK_COUNT        0x00002000
#define SSH_FILEXFER_ATTR_UNTRANSLATED_NAME 0x00004000
#define SSH_FILEXFER_ATTR_CTIME             0x00008000
#define SSH_FILEXFER_ATTR_EXTENDED          0x80000000

/* File types */

#define SSH_FILEXFER_TYPE_REGULAR          1
#define SSH_FILEXFER_TYPE_DIRECTORY        2
#define SSH_FILEXFER_TYPE_SYMLINK          3
#define SSH_FILEXFER_TYPE_SPECIAL          4
#define SSH_FILEXFER_TYPE_UNKNOWN          5
#define SSH_FILEXFER_TYPE_SOCKET           6
#define SSH_FILEXFER_TYPE_CHAR_DEVICE      7
#define SSH_FILEXFER_TYPE_BLOCK_DEVICE     8
#define SSH_FILEXFER_TYPE_FIFO             9

/* Attrib bits */

#define SSH_FILEXFER_ATTR_FLAGS_READONLY         0x00000001
#define SSH_FILEXFER_ATTR_FLAGS_SYSTEM           0x00000002
#define SSH_FILEXFER_ATTR_FLAGS_HIDDEN           0x00000004
#define SSH_FILEXFER_ATTR_FLAGS_CASE_INSENSITIVE 0x00000008
#define SSH_FILEXFER_ATTR_FLAGS_ARCHIVE          0x00000010
#define SSH_FILEXFER_ATTR_FLAGS_ENCRYPTED        0x00000020
#define SSH_FILEXFER_ATTR_FLAGS_COMPRESSED       0x00000040
#define SSH_FILEXFER_ATTR_FLAGS_SPARSE           0x00000080
#define SSH_FILEXFER_ATTR_FLAGS_APPEND_ONLY      0x00000100
#define SSH_FILEXFER_ATTR_FLAGS_IMMUTABLE        0x00000200
#define SSH_FILEXFER_ATTR_FLAGS_SYNC             0x00000400
#define SSH_FILEXFER_ATTR_FLAGS_TRANSLATION_ERR  0x00000800

#define SSH_FX_OK                            0
#define SSH_FX_EOF                           1
#define SSH_FX_NO_SUCH_FILE                  2
#define SSH_FX_PERMISSION_DENIED             3
#define SSH_FX_FAILURE                       4
#define SSH_FX_BAD_MESSAGE                   5
#define SSH_FX_NO_CONNECTION                 6
#define SSH_FX_CONNECTION_LOST               7
#define SSH_FX_OP_UNSUPPORTED                8
#define SSH_FX_INVALID_HANDLE                9
#define SSH_FX_NO_SUCH_PATH                  10
#define SSH_FX_FILE_ALREADY_EXISTS           11
#define SSH_FX_WRITE_PROTECT                 12
#define SSH_FX_NO_MEDIA                      13
#define SSH_FX_NO_SPACE_ON_FILESYSTEM        14
#define SSH_FX_QUOTA_EXCEEDED                15
#define SSH_FX_UNKNOWN_PRINCIPAL             16
#define SSH_FX_LOCK_CONFLICT                 17
#define SSH_FX_DIR_NOT_EMPTY                 18
#define SSH_FX_NOT_A_DIRECTORY               19
#define SSH_FX_INVALID_FILENAME              20
#define SSH_FX_LINK_LOOP                     21
#define SSH_FX_CANNOT_DELETE                 22
#define SSH_FX_INVALID_PARAMETER             23
#define SSH_FX_FILE_IS_A_DIRECTORY           24
#define SSH_FX_BYTE_RANGE_LOCK_CONFLICT      25
#define SSH_FX_BYTE_RANGE_LOCK_REFUSED       26
#define SSH_FX_DELETE_PENDING                27
#define SSH_FX_FILE_CORRUPT                  28
#define SSH_FX_OWNER_INVALID                 29
#define SSH_FX_GROUP_INVALID                 30
#define SSH_FX_NO_MATCHING_BYTE_RANGE_LOCK   31
