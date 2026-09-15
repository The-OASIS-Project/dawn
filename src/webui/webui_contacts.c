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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * WebUI Contacts Management — per-user contact CRUD via WebSocket.
 */

#include "webui/webui_contacts.h"

#include <json-c/json.h>
#include <string.h>

#include "image_store.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_db.h"
#include "webui/webui_internal.h"

#define MAX_CONTACTS_RESULTS 100
#define MAX_CONTACT_VALUE_LEN 255
#define MAX_CONTACT_LABEL_LEN 31

/* =============================================================================
 * Helpers
 * ============================================================================= */

static bool is_valid_field_type(const char *ft) {
   return ft &&
          (strcmp(ft, "email") == 0 || strcmp(ft, "phone") == 0 || strcmp(ft, "address") == 0);
}

/**
 * @brief Validate and normalize a contact value based on field type.
 *
 * For email: checks user@domain.tld format.
 * For phone: strips formatting, normalizes to E.164 (+1 prepended for 10-digit US numbers).
 * For address: no validation.
 *
 * @param field_type  "email", "phone", or "address"
 * @param value       Raw input value
 * @param out         Output buffer for normalized value
 * @param out_size    Size of output buffer
 * @param err_msg     Output: error message if validation fails
 * @return true if valid, false if rejected (err_msg set)
 */
static bool validate_contact_value(const char *field_type,
                                   const char *value,
                                   char *out,
                                   size_t out_size,
                                   const char **err_msg) {
   snprintf(out, out_size, "%s", value);

   if (strcmp(field_type, "email") == 0) {
      const char *at = strchr(out, '@');
      if (!at || at == out || *(at + 1) == '\0') {
         *err_msg = "Invalid email format (expected user@domain.com)";
         return false;
      }
      /* Require at least one char after the dot in domain */
      const char *dot = strchr(at + 1, '.');
      if (!dot || *(dot + 1) == '\0') {
         *err_msg = "Invalid email format (expected user@domain.com)";
         return false;
      }
      /* Normalize: lowercase the entire email */
      for (char *p = out; *p; p++) {
         if (*p >= 'A' && *p <= 'Z') {
            *p = *p + ('a' - 'A');
         }
      }
      return true;
   }

   if (strcmp(field_type, "phone") == 0) {
      char stripped[MAX_CONTACT_VALUE_LEN + 1];
      size_t j = 0;
      for (size_t i = 0; out[i] && j < sizeof(stripped) - 1; i++) {
         char c = out[i];
         if (c == '+' || (c >= '0' && c <= '9')) {
            stripped[j++] = c;
         } else if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.') {
            continue;
         } else {
            *err_msg = "Phone number contains invalid characters";
            return false;
         }
      }
      stripped[j] = '\0';

      /* Reject + in non-leading positions */
      for (size_t i = 1; i < j; i++) {
         if (stripped[i] == '+') {
            *err_msg = "Phone number contains '+' in invalid position";
            return false;
         }
      }

      /* Each branch bounds j (the stripped digit count) to a small exact value,
       * so the E.164 form always fits in out (MAX_CONTACT_VALUE_LEN-sized); the
       * compiler can't carry the branch constraint into strlen(stripped), so it
       * flags a truncation that can't occur. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
      if (stripped[0] == '+') {
         if (j < 8 || j > 16) {
            *err_msg = "Phone number must be 7-15 digits after +";
            return false;
         }
         snprintf(out, out_size, "%s", stripped);
      } else if (j == 11 && stripped[0] == '1') {
         snprintf(out, out_size, "+%s", stripped);
      } else if (j == 10) {
         snprintf(out, out_size, "+1%s", stripped);
      } else {
         *err_msg = "Invalid phone number. Enter 10-digit US number or +country code with number.";
         return false;
      }
#pragma GCC diagnostic pop
      return true;
   }

   return true; /* address — no validation */
}

/* Serialize contact_result_t to JSON object */

static json_object *contact_to_json(const contact_result_t *c) {
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "contact_id", json_object_new_int64(c->contact_id));
   json_object_object_add(obj, "entity_id", json_object_new_int64(c->entity_id));
   json_object_object_add(obj, "entity_name", json_object_new_string(c->entity_name));
   json_object_object_add(obj, "field_type", json_object_new_string(c->field_type));
   json_object_object_add(obj, "value", json_object_new_string(c->value));
   json_object_object_add(obj, "label", json_object_new_string(c->label));
   if (c->photo_id[0]) {
      json_object_object_add(obj, "photo_id", json_object_new_string(c->photo_id));
   }
   return obj;
}

/* =============================================================================
 * List Contacts
 * ============================================================================= */

void handle_contacts_list(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("contacts_list_response"));
   json_object *resp_payload = json_object_new_object();

   /* Optional field_type filter */
   const char *field_type = NULL;
   if (payload) {
      json_object *ft_obj;
      if (json_object_object_get_ex(payload, "field_type", &ft_obj))
         field_type = json_object_get_string(ft_obj);
   }

   /* Optional pagination */
   int limit = 20;
   int offset = 0;
   if (payload) {
      json_object *lim_obj, *off_obj;
      if (json_object_object_get_ex(payload, "limit", &lim_obj))
         limit = json_object_get_int(lim_obj);
      if (json_object_object_get_ex(payload, "offset", &off_obj))
         offset = json_object_get_int(off_obj);
   }
   if (limit <= 0)
      limit = 20;
   if (limit > MAX_CONTACTS_RESULTS)
      limit = MAX_CONTACTS_RESULTS;
   if (offset < 0)
      offset = 0;

   /* Fetch one extra to detect has_more */
   int fetch_count = limit + 1;
   contact_result_t results[fetch_count];
   memset(results, 0, sizeof(contact_result_t) * fetch_count);

   int count = 0;
   int list_rc = contacts_list(conn->auth_user_id, field_type, results, fetch_count, offset,
                               &count);

   if (list_rc != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list contacts"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      bool has_more = (count > limit);
      int return_count = has_more ? limit : count;

      json_object *arr = json_object_new_array();
      for (int i = 0; i < return_count; i++) {
         json_object_array_add(arr, contact_to_json(&results[i]));
      }
      json_object_object_add(resp_payload, "contacts", arr);
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(has_more));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Search Contacts
 * ============================================================================= */

void handle_contacts_search(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("contacts_search_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      goto done;
   }

   json_object *name_obj;
   if (!json_object_object_get_ex(payload, "name", &name_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing 'name' field"));
      goto done;
   }

   const char *name = json_object_get_string(name_obj);

   const char *field_type = NULL;
   json_object *ft_obj;
   if (json_object_object_get_ex(payload, "field_type", &ft_obj))
      field_type = json_object_get_string(ft_obj);

   contact_result_t results[20];
   int count = 0;
   int find_rc = contacts_find(conn->auth_user_id, name, field_type, results, 20, &count);

   if (find_rc != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Search failed"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object *arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object_array_add(arr, contact_to_json(&results[i]));
      }
      json_object_object_add(resp_payload, "contacts", arr);
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Add Contact
 * ============================================================================= */

void handle_contacts_add(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("contacts_add_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      goto done;
   }

   json_object *ft_obj, *val_obj;
   if (!json_object_object_get_ex(payload, "field_type", &ft_obj) ||
       !json_object_object_get_ex(payload, "value", &val_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing required fields: field_type, value"));
      goto done;
   }

   const char *field_type = json_object_get_string(ft_obj);
   const char *value = json_object_get_string(val_obj);

   const char *label = "";
   json_object *lbl_obj;
   if (json_object_object_get_ex(payload, "label", &lbl_obj))
      label = json_object_get_string(lbl_obj);

   /* Validate field_type */
   if (!is_valid_field_type(field_type)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "field_type must be 'email', 'phone', or 'address'"));
      goto done;
   }

   /* Validate value/label length */
   if (strlen(value) > MAX_CONTACT_VALUE_LEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Value exceeds maximum length (255)"));
      goto done;
   }
   if (label && strlen(label) > MAX_CONTACT_LABEL_LEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Label exceeds maximum length (31)"));
      goto done;
   }

   /* Validate and normalize value by field type */
   char normalized_value[MAX_CONTACT_VALUE_LEN + 1];
   const char *val_err = NULL;
   if (!validate_contact_value(field_type, value, normalized_value, sizeof(normalized_value),
                               &val_err)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string(val_err));
      goto done;
   }
   value = normalized_value;

   /* Resolve entity: use explicit entity_id if provided, otherwise find/create by name */
   int64_t entity_id = -1;
   bool created = false;
   json_object *eid_obj;
   if (json_object_object_get_ex(payload, "entity_id", &eid_obj)) {
      entity_id = json_object_get_int64(eid_obj);
   } else {
      json_object *name_obj;
      if (!json_object_object_get_ex(payload, "entity_name", &name_obj) ||
          !json_object_get_string(name_obj) || !json_object_get_string(name_obj)[0]) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(
                                    "Either 'entity_id' or 'entity_name' is required"));
         goto done;
      }
      const char *entity_name = json_object_get_string(name_obj);
      char canonical[64];
      memory_make_canonical_name(entity_name, canonical, sizeof(canonical));

      /* Check if force_create flag is set (user already saw disambiguation) */
      bool force_create = false;
      json_object *fc_obj;
      if (json_object_object_get_ex(payload, "force_create", &fc_obj))
         force_create = json_object_get_boolean(fc_obj);

      /* Check for exact canonical match first */
      memory_entity_t exact;
      int exact_rc = memory_db_entity_get_by_name(conn->auth_user_id, canonical, &exact);

      if (exact_rc == MEMORY_DB_SUCCESS) {
         /* Exact match — use existing entity */
         entity_id = exact.id;
      } else if (!force_create) {
         /* No exact match — search for similar person entities */
         memory_entity_t similar[10];
         int sim_count = 0;
         memory_db_entity_search(conn->auth_user_id, entity_name, similar, 10, &sim_count);

         /* Filter to person entities only */
         int person_count = 0;
         memory_entity_t persons[10];
         for (int i = 0; i < sim_count; i++) {
            if (strcmp(similar[i].entity_type, "person") == 0)
               persons[person_count++] = similar[i];
         }

         if (person_count > 0) {
            /* Return candidates for disambiguation */
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "needs_disambiguation",
                                   json_object_new_boolean(1));
            json_object *candidates = json_object_new_array();
            for (int i = 0; i < person_count; i++) {
               json_object *c = json_object_new_object();
               json_object_object_add(c, "id", json_object_new_int64(persons[i].id));
               json_object_object_add(c, "name", json_object_new_string(persons[i].name));
               json_object_object_add(c, "mention_count",
                                      json_object_new_int(persons[i].mention_count));
               json_object_array_add(candidates, c);
            }
            json_object_object_add(resp_payload, "candidates", candidates);
            json_object_object_add(resp_payload, "entered_name",
                                   json_object_new_string(entity_name));
            goto done;
         }

         /* No similar people — create new entity */
         memory_db_entity_upsert(conn->auth_user_id, entity_name, "person", canonical, &created,
                                 &entity_id);
      } else {
         /* force_create — skip disambiguation, create new entity directly */
         memory_db_entity_upsert(conn->auth_user_id, entity_name, "person", canonical, &created,
                                 &entity_id);
      }
   }

   if (entity_id <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to find or create entity"));
      goto done;
   }

   if (contacts_add(conn->auth_user_id, entity_id, field_type, value, label) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to add contact"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entity_created", json_object_new_boolean(created));
      json_object_object_add(resp_payload, "entity_id", json_object_new_int64(entity_id));
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Update Contact
 * ============================================================================= */

void handle_contacts_update(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("contacts_update_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      goto done;
   }

   json_object *id_obj, *ft_obj, *val_obj;
   if (!json_object_object_get_ex(payload, "contact_id", &id_obj) ||
       !json_object_object_get_ex(payload, "field_type", &ft_obj) ||
       !json_object_object_get_ex(payload, "value", &val_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "Missing required fields: contact_id, field_type, value"));
      goto done;
   }

   int64_t contact_id = json_object_get_int64(id_obj);
   const char *field_type = json_object_get_string(ft_obj);
   const char *value = json_object_get_string(val_obj);

   const char *label = "";
   json_object *lbl_obj;
   if (json_object_object_get_ex(payload, "label", &lbl_obj))
      label = json_object_get_string(lbl_obj);

   /* Validate field_type */
   if (!is_valid_field_type(field_type)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "field_type must be 'email', 'phone', or 'address'"));
      goto done;
   }

   /* Validate value/label length */
   if (strlen(value) > MAX_CONTACT_VALUE_LEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Value exceeds maximum length (255)"));
      goto done;
   }
   if (label && strlen(label) > MAX_CONTACT_LABEL_LEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Label exceeds maximum length (31)"));
      goto done;
   }

   /* Validate and normalize value by field type */
   char normalized_value[MAX_CONTACT_VALUE_LEN + 1];
   const char *val_err = NULL;
   if (!validate_contact_value(field_type, value, normalized_value, sizeof(normalized_value),
                               &val_err)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string(val_err));
      goto done;
   }
   value = normalized_value;

   if (contacts_update(conn->auth_user_id, contact_id, field_type, value, label) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to update contact"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete Contact
 * ============================================================================= */

void handle_contacts_delete(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("contacts_delete_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      goto done;
   }

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "contact_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'contact_id' field"));
      goto done;
   }

   int64_t contact_id = json_object_get_int64(id_obj);

   if (contacts_delete(conn->auth_user_id, contact_id) != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete contact"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Search Entities (typeahead for contact add modal)
 * ============================================================================= */

void handle_contacts_search_entities(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("contacts_search_entities_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      goto search_entities_done;
   }

   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'query' field"));
      goto search_entities_done;
   }

   const char *query = json_object_get_string(query_obj);
   if (!query || !query[0]) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entities", json_object_new_array());
      goto search_entities_done;
   }

   memory_entity_t entities[10];
   int count = 0;
   memory_db_entity_search(conn->auth_user_id, query, entities, 10, &count);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object *arr = json_object_new_array();

   for (int i = 0; i < count; i++) {
      /* Only return person entities for contact association */
      if (strcmp(entities[i].entity_type, "person") != 0)
         continue;
      json_object *obj = json_object_new_object();
      json_object_object_add(obj, "id", json_object_new_int64(entities[i].id));
      json_object_object_add(obj, "name", json_object_new_string(entities[i].name));
      json_object_object_add(obj, "entity_type", json_object_new_string(entities[i].entity_type));
      json_object_object_add(obj, "mention_count", json_object_new_int(entities[i].mention_count));
      json_object_array_add(arr, obj);
   }

   json_object_object_add(resp_payload, "entities", arr);

search_entities_done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Set Entity Photo
 * ============================================================================= */

void handle_entity_set_photo(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;
   if (!payload)
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("entity_set_photo_response"));
   json_object *resp_payload = json_object_new_object();

   /* Required: entity_id */
   json_object *entity_id_obj;
   if (!json_object_object_get_ex(payload, "entity_id", &entity_id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'entity_id' field"));
      goto set_photo_done;
   }
   int64_t entity_id = json_object_get_int64(entity_id_obj);

   /* Optional: photo_id (null/empty = clear photo) */
   const char *photo_id = NULL;
   json_object *photo_id_obj;
   if (json_object_object_get_ex(payload, "photo_id", &photo_id_obj)) {
      photo_id = json_object_get_string(photo_id_obj);
   }

   /* If setting a photo, verify image exists and belongs to this user */
   if (photo_id && photo_id[0]) {
      image_metadata_t meta;
      int img_rc = image_store_get_metadata(photo_id, &meta);
      if (img_rc != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Image not found"));
         goto set_photo_done;
      }
      if (meta.user_id != conn->auth_user_id) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Image does not belong to you"));
         goto set_photo_done;
      }
   }

   /* Fetch old photo_id before updating (for retention downgrade) */
   char old_photo_id[32] = "";
   memory_db_entity_get_photo(conn->auth_user_id, entity_id, old_photo_id, sizeof(old_photo_id));

   int rc = memory_db_entity_set_photo(conn->auth_user_id, entity_id, photo_id);
   if (rc == MEMORY_DB_SUCCESS) {
      /* Upgrade new photo to permanent, downgrade old to default.
       * Non-critical — photo still works if retention update fails. */
      if (photo_id && photo_id[0]) {
         int ret_rc = image_store_update_retention(photo_id, conn->auth_user_id,
                                                   IMAGE_RETAIN_PERMANENT);
         if (ret_rc != IMAGE_STORE_SUCCESS) {
            OLOG_WARNING("webui_contacts: failed to upgrade retention for %s (rc=%d)", photo_id,
                         ret_rc);
         }
      }
      if (old_photo_id[0] && (!photo_id || strcmp(old_photo_id, photo_id) != 0)) {
         int ret_rc = image_store_update_retention(old_photo_id, conn->auth_user_id,
                                                   IMAGE_RETAIN_DEFAULT);
         if (ret_rc != IMAGE_STORE_SUCCESS) {
            OLOG_WARNING("webui_contacts: failed to downgrade retention for %s (rc=%d)",
                         old_photo_id, ret_rc);
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entity_id", json_object_new_int64(entity_id));
      if (photo_id && photo_id[0]) {
         json_object_object_add(resp_payload, "photo_id", json_object_new_string(photo_id));
      }
   } else if (rc == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Entity not found or not owned by you"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Database error"));
   }

set_photo_done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Ensure Entity (find or create by name — lightweight upsert for photo-only flow)
 * ============================================================================= */

void handle_entity_ensure(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;
   if (!payload)
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("entity_ensure_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *name_obj;
   if (!json_object_object_get_ex(payload, "name", &name_obj) ||
       !json_object_get_string(name_obj) || !json_object_get_string(name_obj)[0]) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing 'name' field"));
      goto ensure_done;
   }

   const char *name = json_object_get_string(name_obj);
   char canonical[64];
   memory_make_canonical_name(name, canonical, sizeof(canonical));

   bool created = false;
   int64_t entity_id = 0;
   int upsert_rc = memory_db_entity_upsert(conn->auth_user_id, name, "person", canonical, &created,
                                           &entity_id);
   if (upsert_rc != MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create entity"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entity_id", json_object_new_int64(entity_id));
      json_object_object_add(resp_payload, "created", json_object_new_boolean(created));
   }

ensure_done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
