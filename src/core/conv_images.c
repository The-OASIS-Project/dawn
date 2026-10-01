/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * The images a conversation owns, and what goes when it (or its user) does.
 * See conv_images.h.
 */

#include "core/conv_images.h"

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "blob_store.h"
#include "document_original_store.h"
#include "image_store.h"
#include "logging.h"

int conv_images_delete_conversation(int64_t conv_id, int user_id) {
   conv_image_files_t files = { 0 };
   const int rc = conv_db_delete_ex(conv_id, user_id, user_id == 0, CONV_IMAGES_DELETE, &files);
   /* The rows are gone with the conversation; their files go now, with no
    * lock held. */
   for (int i = 0; i < files.count; i++) {
      (void)image_store_unlink_file(files.names[i]);
   }
   if (files.count > 0) {
      OLOG_INFO("conv_images: deleted %d image(s) of conversation %lld", files.count,
                (long long)conv_id);
   }
   conv_image_files_free(&files);
   return rc;
}

bool conv_images_purge_user(int user_id) {
   if (user_id <= 0) {
      return false;
   }
   bool ok = true;
   if (image_store_is_ready() && image_store_delete_user(user_id) != IMAGE_STORE_SUCCESS) {
      OLOG_WARNING("conv_images: image purge for user %d incomplete (rows/files may remain)",
                   user_id);
      ok = false;
   }
   if (document_originals_ready() && document_original_delete_user(user_id) != BLOB_STORE_SUCCESS) {
      OLOG_WARNING("conv_images: document-original purge for user %d incomplete", user_id);
      ok = false;
   }
   return ok;
}
