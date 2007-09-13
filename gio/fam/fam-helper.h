#ifndef __FAM_HELPER_H__
#define __FAM_HELPER_H__

typedef struct _fam_sub fam_sub;

fam_sub*  _fam_sub_add    (const gchar* pathname,
			   gboolean     directory,
			   gpointer     user_data);
gboolean  _fam_sub_cancel (fam_sub* sub);
void      _fam_sub_free   (fam_sub* sub);

#endif /* __FAM_HELPER_H__ */
