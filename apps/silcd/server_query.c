/*

  server_query.c 

  Author: Pekka Riikonen <priikone@silcnet.org>

  Copyright (C) 2002 Pekka Riikonen

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

*/
/* $Id$ */

/* XXX TODO Requested Attributes to WHOIS */

#include "serverincludes.h"
#include "server_internal.h"

typedef struct {
  SilcSocketConnection sock;	    /* Connection of this query */
  unsigned char **arg;		    /* Query argument */
  SilcUInt32 *arg_lens;		    /* Query argument lengths */
  SilcUInt32 *arg_types;	    /* Query argument types */
  SilcUInt32 argc;		    /* Number of query arguments */
  SilcUInt32 timeout;		    /* Max timeout for query to complete */
  SilcUInt16 ident;		    /* Query command identifier */
} *SilcServerQueryList;

/* Represents an SILC ID */
typedef struct {
  void *id;			    /* ID */
  SilcIdType id_type;		    /* ID type */
  SilcUInt16 ident;		    /* Command identifier */
} *SilcServerQueryID;

/* Represents one error occurred during query */
typedef struct {
  void *id;			    /* ID */
  SilcIdType id_type;		    /* ID type */
  SilcUInt16 index;		    /* Index to IDs */
  unsigned int from_cmd : 1;   	    /* TRUE if `index' is from command args,
				       otherwise from query->ids */
  unsigned int error : 7;	    /* The actual error (SilcStatus) */
} *SilcServerQueryError;

/* Query session context */
typedef struct {
  /* Queried data */
  char *nickname;		    /* Queried nickname */
  char *nick_server;		    /* Queried nickname's server */
  char *server_name;		    /* Queried server name */
  char *channel_name;		    /* Queried channel name */
  SilcServerQueryID ids;	    /* Queried IDs */
  SilcUInt32 ids_count;		    /* number of queried IDs */
  SilcUInt32 reply_count;	    /* Requested reply count */
  SilcDList attrs;		    /* Requested Attributes in WHOIS */

  /* Query session data */
  SilcServerCommandContext cmd;	    /* Command context for query */
  SilcServerQueryList querylist;    /* Temporary query list context */
  SilcServerQueryID queries;	    /* Ongoing queries */
  SilcServerQueryError errors;	    /* Query errors */
  SilcUInt16 querylist_count;	    /* Number of query lists */
  SilcUInt16 queries_count;	    /* Number of ongoing queries */
  SilcUInt16 queries_left;	    /* Number of ongoing queries left */
  SilcUInt16 errors_count;	    /* number of errors */
  unsigned int querycmd : 7;	    /* Query command (SilcCommand) */
  unsigned int resolved : 1;	    /* TRUE if normal server has resolved
				       information from router */
} *SilcServerQuery;

void silc_server_query_free(SilcServerQuery query);
void silc_server_query_send_error(SilcServer server,
				  SilcServerQuery query,
				  SilcStatus error, ...);
void silc_server_query_add_error(SilcServer server,
				 SilcServerQuery query,
				 bool from_cmd,
				 SilcUInt32 index,
				 SilcStatus error);
void silc_server_query_add_error_id(SilcServer server,
				    SilcServerQuery query,
				    SilcStatus error,
				    void *id, SilcIdType id_type);
void silc_server_query_send_router(SilcServer server, SilcServerQuery query);
void silc_server_query_send_router_reply(void *context, void *reply);
void silc_server_query_parse(SilcServer server, SilcServerQuery query);
void silc_server_query_process(SilcServer server, SilcServerQuery query,
			       bool resolve);
void silc_server_query_resolve(SilcServer server, SilcServerQuery query,
			       SilcSocketConnection sock,
			       SilcClientEntry client_entry);
void silc_server_query_resolve_reply(void *context, void *reply);
void silc_server_query_send_reply(SilcServer server,
				  SilcServerQuery query,
				  SilcClientEntry *clients,
				  SilcUInt32 clients_count,
				  SilcServerEntry *servers,
				  SilcUInt32 servers_count,
				  SilcChannelEntry *channels,
				  SilcUInt32 channels_count);
unsigned char *silc_server_query_reply_attrs(SilcServer server,
					     SilcServerQuery query,
					     SilcUInt32 *attrs_len);

/* Free the query context structure and all allocated resources. */

void silc_server_query_free(SilcServerQuery query)
{
  int i;

  silc_server_command_free(query->cmd);

  for (i = 0; i < query->queries_count; i++)
    silc_free(query->queries[i].id);
  silc_free(query->queries);

  silc_free(query->nickname);
  silc_free(query->nick_server);
  silc_free(query->server_name);
  silc_free(query->channel_name);

  for (i = 0; i < query->ids_count; i++)
    silc_free(query->ids[i].id);
  silc_free(query->ids);

  if (query->attrs)
    silc_attribute_payload_list_free(query->attrs);

  for (i = 0; i < query->errors_count; i++)
    silc_free(query->errors[i].id);
  silc_free(query->errors);

  memset(query, 'F', sizeof(*query));
  silc_free(query);
}

/* Send error reply indicated by the `error' to the original sender of
   the query. */

void silc_server_query_send_error(SilcServer server,
				  SilcServerQuery query,
				  SilcStatus error, ...)
{
  va_list va;
  unsigned char *data = NULL;
  SilcUInt32 data_len = 0, data_type = 0, argc = 0;

  va_start(va, error);
  data_type = va_arg(va, SilcUInt32);
  if (data_type) {
    argc = 1;
    data = va_arg(va, unsigned char *);
    data_len = va_arg(va, SilcUInt32);
  }

  SILC_LOG_DEBUG(("ERROR: %s (%d)", silc_get_status_message(error), error));

  /* Send the command reply with error */
  silc_server_send_command_reply(server, query->cmd->sock,
				 query->querycmd, error, 0, 
				 silc_command_get_ident(query->cmd->payload),
				 argc, data_type, data, data_len);
  va_end(va);
}

/* Add error to error list.  Multiple errors may occur during the query
   processing and this function can be used to add one error.  The
   `index' is the index to the command context which includes the argument
   which caused the error, or it is the index to query->ids, depending
   on value of `from_cmd'. */

void silc_server_query_add_error(SilcServer server,
				 SilcServerQuery query,
				 bool from_cmd,
				 SilcUInt32 index,
				 SilcStatus error)
{
  query->errors = silc_realloc(query->errors, sizeof(*query->errors) *
			       (query->errors_count + 1));
  if (!query->errors)
    return;
  query->errors[query->errors_count].index = index;
  query->errors[query->errors_count].from_cmd = from_cmd;
  query->errors[query->errors_count].error = error;
  query->errors[query->errors_count].id = NULL;
  query->errors[query->errors_count].id_type = 0;
  query->errors_count++;
}

/* Same as silc_server_query_add_error but adds the ID data to be used
   with error sending with this error type. */

void silc_server_query_add_error_id(SilcServer server,
				    SilcServerQuery query,
				    SilcStatus error,
				    void *id, SilcIdType id_type)
{
  query->errors = silc_realloc(query->errors, sizeof(*query->errors) *
			       (query->errors_count + 1));
  if (!query->errors)
    return;
  query->errors[query->errors_count].index = 0;
  query->errors[query->errors_count].from_cmd = FALSE;
  query->errors[query->errors_count].error = error;
  query->errors[query->errors_count].id = silc_id_dup(id, id_type);
  query->errors[query->errors_count].id_type = id_type;
  query->errors_count++;
}

/* Processes query as command.  The `query' is the command that is
   being processed indicated by the `cmd'.  The `query' can be one of
   the following: SILC_COMMAND_WHOIS, SILC_COMMAND_WHOWAS or
   SILC_COMMAND_IDENTIFY.  This function handles the reply sending
   to the entity who sent this query to us automatically.  Returns
   TRUE if the query is being processed or FALSE on error. */

bool silc_server_query_command(SilcServer server, SilcCommand querycmd,
			       SilcServerCommandContext cmd)
{
  SilcServerQuery query;

  SILC_LOG_DEBUG(("Query %s command", silc_get_command_name(querycmd)));

  query = silc_calloc(1, sizeof(*query));
  query->querycmd = querycmd;
  query->cmd = silc_server_command_dup(cmd);

  switch (querycmd) {

  case SILC_COMMAND_WHOIS:
    /* If we are normal server and query contains nickname, send it
       directly to router. */
    if (server->server_type == SILC_SERVER && !server->standalone &&
	cmd->sock != SILC_PRIMARY_ROUTE(server) &&
	silc_argument_get_arg_type(cmd->args, 1, NULL)) {
      silc_server_query_send_router(server, query);
      return TRUE;
    }
    break;

  case SILC_COMMAND_WHOWAS:
    /* WHOWAS query is always sent to router if we are normal server */
    if (server->server_type == SILC_SERVER && !server->standalone &&
	cmd->sock != SILC_PRIMARY_ROUTE(server)) {
      silc_server_query_send_router(server, query);
      return TRUE;
    }
    break;

  case SILC_COMMAND_IDENTIFY:
    /* If we are normal server and query does not contain IDs, send it
       directly to router (it contains nickname, server name or channel
       name). */
    if (server->server_type == SILC_SERVER && !server->standalone &&
	cmd->sock != SILC_PRIMARY_ROUTE(server) &&
	!silc_argument_get_arg_type(cmd->args, 5, NULL)) {
      silc_server_query_send_router(server, query);
      return TRUE;
    }
    break;

  default:
    SILC_LOG_ERROR(("Bad query using %d command", querycmd));
    silc_server_query_free(query);
    return FALSE;
  }

  /* Now parse the request */
  silc_server_query_parse(server, query);

  return TRUE;
}

/* Send the received query to our primary router since we could not
   handle the query directly.  We will reprocess the query after our
   router replies back. */

void silc_server_query_send_router(SilcServer server, SilcServerQuery query)
{
  SilcBuffer tmpbuf;
  SilcUInt16 old_ident;

  SILC_LOG_DEBUG(("Forwarding the query to router for processing"));

  /* Send WHOIS command to our router */
  old_ident = silc_command_get_ident(query->cmd->payload);
  silc_command_set_ident(query->cmd->payload, ++server->cmd_ident);
  tmpbuf = silc_command_payload_encode_payload(query->cmd->payload);
  silc_server_packet_send(server, 
			  SILC_PRIMARY_ROUTE(server),
			  SILC_PACKET_COMMAND, 0,
			  tmpbuf->data, tmpbuf->len, TRUE);
  silc_command_set_ident(query->cmd->payload, old_ident);
  silc_buffer_free(tmpbuf);

  query->resolved = TRUE;

  /* Continue parsing the query after received reply from router */
  silc_server_command_pending(server, query->querycmd, server->cmd_ident,
			      silc_server_query_send_router_reply, query);
}

/* Reply callback called after primary router has replied to our initial
   sending of the query to it.  We will proceed the query in this function. */

void silc_server_query_send_router_reply(void *context, void *reply)
{
  SilcServerQuery query = context;
  SilcServer server = query->cmd->server;
  SilcServerCommandReplyContext cmdr = reply;

  SILC_LOG_DEBUG(("Received reply from router to query"));

  /* Check if router sent error reply */
  if (cmdr && !silc_command_get_status(cmdr->payload, NULL, NULL)) {
    SilcBuffer buffer;

    SILC_LOG_DEBUG(("Sending error to original query"));

    /* Send the same command reply payload which contains the error */
    silc_command_set_command(cmdr->payload, query->querycmd);
    silc_command_set_ident(cmdr->payload,
			   silc_command_get_ident(query->cmd->payload));
    buffer = silc_command_payload_encode_payload(cmdr->payload);
    silc_server_packet_send(server, query->cmd->sock,
			    SILC_PACKET_COMMAND_REPLY, 0, 
			    buffer->data, buffer->len, FALSE);
    silc_buffer_free(buffer);
    silc_server_query_free(query);
    return;
  }

  /* Continue with parsing */
  silc_server_query_parse(server, query);
}

/* Parse the command query and start processing the queries in detail. */

void silc_server_query_parse(SilcServer server, SilcServerQuery query)
{
  SilcServerCommandContext cmd = query->cmd;
  unsigned char *tmp;
  SilcUInt32 tmp_len, argc = silc_argument_get_arg_num(cmd->args);
  void *id;
  SilcIdType id_type;
  int i;

  SILC_LOG_DEBUG(("Parsing %s query",
		  silc_get_command_name(query->querycmd)));

  switch (query->querycmd) {

  case SILC_COMMAND_WHOIS:
    /* Get Client IDs if present. Take IDs always instead of nickname. */
    tmp = silc_argument_get_arg_type(cmd->args, 4, &tmp_len);
    if (!tmp) {

      /* Get nickname */
      tmp = silc_argument_get_arg_type(cmd->args, 1, &tmp_len);
      if (!tmp) {
	silc_server_query_send_error(server, query,
				     SILC_STATUS_ERR_NOT_ENOUGH_PARAMS, 0);
	silc_server_query_free(query);
	return;
      }

      /* Get the nickname@server string and parse it */
      if (tmp_len > 128 ||
	  !silc_parse_userfqdn(tmp, &query->nickname, &query->nick_server)) {
	silc_server_query_send_error(server, query,
				     SILC_STATUS_ERR_BAD_NICKNAME, 0);
	silc_server_query_free(query);
	return;
      }

    } else {
      /* Parse the IDs included in the query */
      query->ids = silc_calloc(argc, sizeof(*query->ids));

      for (i = 0; i < argc; i++) {
	tmp = silc_argument_get_arg_type(cmd->args, i + 4, &tmp_len);
	if (!tmp)
	  continue;

	id = silc_id_payload_parse_id(tmp, tmp_len, NULL);
	if (!id) {
	  silc_server_query_add_error(server, query, TRUE, i + 4,
				      SILC_STATUS_ERR_BAD_CLIENT_ID);
	  continue;
	}

	/* Normal server must check whether this ID exist, and if not then
	   send the query to router, unless done so already */
	if (server->server_type == SILC_SERVER && !query->resolved) {
	  if (!silc_idlist_find_client_by_id(server->local_list,
					     id, TRUE, NULL)) {
	    if (cmd->sock->type != SILC_SOCKET_TYPE_CLIENT ||
		!silc_idlist_find_client_by_id(server->global_list,
					       id, TRUE, NULL)) {
	      silc_server_query_send_router(server, query);
	      for (i = 0; i < query->ids_count; i++)
		silc_free(query->ids[i].id);
	      silc_free(query->ids);
	      silc_free(id);
	      return;
	    }
	  }
	}

	query->ids[query->ids_count].id = id;
	query->ids[query->ids_count].id_type = SILC_ID_CLIENT;
	query->ids_count++;
      }
    }

    /* Get the max count of reply messages allowed */
    tmp = silc_argument_get_arg_type(cmd->args, 2, &tmp_len);
    if (tmp && tmp_len == sizeof(SilcUInt32))
      SILC_GET32_MSB(query->reply_count, tmp);

    /* Get requested attributes if set */
    tmp = silc_argument_get_arg_type(cmd->args, 3, &tmp_len);
    if (tmp)
      query->attrs = silc_attribute_payload_parse_list(tmp, tmp_len);
    break;

  case SILC_COMMAND_WHOWAS:
    /* Get nickname */
    tmp = silc_argument_get_arg_type(cmd->args, 1, &tmp_len);
    if (!tmp) {
      silc_server_query_send_error(server, query,
				   SILC_STATUS_ERR_NOT_ENOUGH_PARAMS, 0);
      silc_server_query_free(query);
      return;
    }

    /* Get the nickname@server string and parse it */
    if (tmp_len > 128 ||
	!silc_parse_userfqdn(tmp, &query->nickname, &query->nick_server)) {
      silc_server_query_send_error(server, query,
				   SILC_STATUS_ERR_BAD_NICKNAME, 0);
      silc_server_query_free(query);
      return;
    }

    /* Get the max count of reply messages allowed */
    tmp = silc_argument_get_arg_type(cmd->args, 2, &tmp_len);
    if (tmp && tmp_len == sizeof(SilcUInt32))
      SILC_GET32_MSB(query->reply_count, tmp);
    break;

  case SILC_COMMAND_IDENTIFY:
    /* Get IDs if present. Take IDs always instead of names. */
    tmp = silc_argument_get_arg_type(cmd->args, 5, &tmp_len);
    if (!tmp) {

      /* Try get nickname */
      tmp = silc_argument_get_arg_type(cmd->args, 1, &tmp_len);
      if (tmp) {
	/* Get the nickname@server string and parse it */
	if (tmp_len > 128 ||
	    !silc_parse_userfqdn(tmp, &query->nickname, &query->nick_server))
	  silc_server_query_add_error(server, query, TRUE, 1,
				      SILC_STATUS_ERR_BAD_NICKNAME);
      }

      /* Try get server name */
      tmp = silc_argument_get_arg_type(cmd->args, 2, &tmp_len);
      if (tmp)
	query->server_name = silc_memdup(tmp, tmp_len);

      /* Get channel name */
      tmp = silc_argument_get_arg_type(cmd->args, 3, &tmp_len);
      if (tmp && tmp_len <= 256)
	query->channel_name = silc_memdup(tmp, tmp_len);

      if (!query->nickname && !query->server_name && !query->channel_name) {
	silc_server_query_send_error(server, query,
				     SILC_STATUS_ERR_NOT_ENOUGH_PARAMS, 0);
	silc_server_query_free(query);
	return;
      }

    } else {
      /* Parse the IDs included in the query */
      query->ids = silc_calloc(argc, sizeof(*query->ids));

      for (i = 0; i < argc; i++) {
	tmp = silc_argument_get_arg_type(cmd->args, i + 5, &tmp_len);
	if (!tmp)
	  continue;

	id = silc_id_payload_parse_id(tmp, tmp_len, &id_type);
	if (!id) {
	  silc_server_query_add_error(server, query, TRUE, i + 5,
				      SILC_STATUS_ERR_BAD_CLIENT_ID);
	  continue;
	}

	/* Normal server must check whether this ID exist, and if not then
	   send the query to router, unless done so already */
	if (server->server_type == SILC_SERVER && !query->resolved) {
	  if (!silc_idlist_find_client_by_id(server->local_list,
					     id, TRUE, NULL)) {
	    if (cmd->sock->type != SILC_SOCKET_TYPE_CLIENT ||
		!silc_idlist_find_client_by_id(server->global_list,
					       id, TRUE, NULL)) {
	      silc_server_query_send_router(server, query);
	      for (i = 0; i < query->ids_count; i++)
		silc_free(query->ids[i].id);
	      silc_free(query->ids);
	      silc_free(id);
	      return;
	    }
	  }
	}

	query->ids[query->ids_count].id = id;
	query->ids[query->ids_count].id_type = id_type;
	query->ids_count++;
      }
    }

    /* Get the max count of reply messages allowed */
    tmp = silc_argument_get_arg_type(cmd->args, 4, &tmp_len);
    if (tmp && tmp_len == sizeof(SilcUInt32))
      SILC_GET32_MSB(query->reply_count, tmp);
    break;
  }

  /* Start processing the query information */
  silc_server_query_process(server, query, TRUE);
}

/* Processes the parsed query.  This does the actual finding of the
   queried information and prepares for sending reply to the original
   sender of the query command. */

void silc_server_query_process(SilcServer server, SilcServerQuery query,
			       bool resolve)
{
  SilcServerCommandContext cmd = query->cmd;
  bool check_global = FALSE;
  void *entry;
  SilcClientEntry *clients = NULL, client_entry;
  SilcChannelEntry *channels = NULL;
  SilcServerEntry *servers = NULL;
  SilcUInt32 clients_count = 0, channels_count = 0, servers_count = 0;
  int i;

  SILC_LOG_DEBUG(("Processing %s query",
		  silc_get_command_name(query->querycmd)));

  /* Check global lists if query is coming from client or we are not
     normal server (we know global information). */
  if (cmd->sock->type == SILC_SOCKET_TYPE_CLIENT)
    check_global = TRUE;
  else if (server->server_type != SILC_SERVER)
    check_global = TRUE;

  if (query->nickname) {
    /* Get all clients matching nickname from local list */
    if (!silc_idlist_get_clients_by_hash(server->local_list, 
					 query->nickname, server->md5hash,
					 &clients, &clients_count))
      silc_idlist_get_clients_by_nickname(server->local_list, 
					  query->nickname,
					  query->nick_server,
					  &clients, &clients_count);

    /* Check global list as well */
    if (check_global) {
      if (!silc_idlist_get_clients_by_hash(server->global_list, 
					   query->nickname, server->md5hash,
					   &clients, &clients_count))
	silc_idlist_get_clients_by_nickname(server->global_list, 
					    query->nickname,
					    query->nick_server,
					    &clients, &clients_count);
    }

    if (!clients)
      silc_server_query_add_error(server, query, TRUE, 1,
				  SILC_STATUS_ERR_NO_SUCH_NICK);
  }

  if (query->server_name) {
    /* Find server by name */
    entry = silc_idlist_find_server_by_name(server->local_list,
					    query->server_name, TRUE, NULL);
    if (!entry && check_global)
      entry = silc_idlist_find_server_by_name(server->global_list,
					      query->server_name, TRUE, NULL);
    if (entry) {
      servers = silc_realloc(servers, sizeof(*servers) * (servers_count + 1));
      servers[servers_count++] = (SilcServerEntry)entry;
    }

    if (!servers)
      silc_server_query_add_error(server, query, TRUE, 2,
				  SILC_STATUS_ERR_NO_SUCH_SERVER);
  }

  if (query->channel_name) {
    /* Find channel by name */
    entry = silc_idlist_find_channel_by_name(server->local_list,
					     query->channel_name, NULL);
    if (!entry && check_global)
      entry = silc_idlist_find_channel_by_name(server->global_list,
					       query->channel_name, NULL);
    if (entry) {
      channels = silc_realloc(channels, sizeof(*channels) *
			      (channels_count + 1));
      channels[channels_count++] = (SilcChannelEntry)entry;
    }

    if (!channels)
      silc_server_query_add_error(server, query, TRUE, 3,
				  SILC_STATUS_ERR_NO_SUCH_CHANNEL);
  }

  if (query->ids_count) {
    /* Find entries by the queried IDs */
    for (i = 0; i < query->ids_count; i++) {
      void *id = query->ids[i].id;
      if (!id)
	continue;

      switch (query->ids[i].id_type) {

      case SILC_ID_CLIENT:
	/* Get client entry */
	entry = silc_idlist_find_client_by_id(server->local_list,
					      id, TRUE, NULL);
	if (!entry && check_global)
	  entry = silc_idlist_find_client_by_id(server->global_list,
						id, TRUE, NULL);
	if (!entry) {
	  silc_server_query_add_error(server, query, FALSE, i,
				      SILC_STATUS_ERR_NO_SUCH_CLIENT_ID);
	  continue;
	}

	clients = silc_realloc(clients, sizeof(*clients) *
			       (clients_count + 1));
	clients[clients_count++] = (SilcClientEntry)entry;
	break;

      case SILC_ID_SERVER:
	/* Get server entry */
	entry = silc_idlist_find_server_by_id(server->local_list,
					      id, TRUE, NULL);
	if (!entry && check_global)
	  entry = silc_idlist_find_server_by_id(server->global_list,
						id, TRUE, NULL);
	if (!entry) {
	  silc_server_query_add_error(server, query, FALSE, i,
				      SILC_STATUS_ERR_NO_SUCH_SERVER_ID);
	  continue;
	}

	servers = silc_realloc(servers, sizeof(*servers) *
			       (servers_count + 1));
	servers[servers_count++] = (SilcServerEntry)entry;
	break;

      case SILC_ID_CHANNEL:
	/* Get channel entry */
	entry = silc_idlist_find_channel_by_id(server->local_list, id, NULL);
	if (!entry && check_global)
	  entry = silc_idlist_find_channel_by_id(server->global_list, id,
						 NULL);
	if (!entry) {
	  silc_server_query_add_error(server, query, FALSE, i,
				      SILC_STATUS_ERR_NO_SUCH_CHANNEL_ID);
	  continue;
	}

	channels = silc_realloc(channels, sizeof(*channels) *
				(channels_count + 1));
	channels[channels_count++] = (SilcChannelEntry)entry;
	break;

      default:
	break;
      }
    }
  }

  /* If nothing was found, then just send the errors */
  if (!clients && !channels && !servers) {
    silc_server_query_send_reply(server, query, NULL, 0, NULL, 0, NULL, 0);
    return;
  }

  /* If caller does not want us to resolve anything (has resolved already)
     then just continue with sending the reply */
  if (!resolve) {
    silc_server_query_send_reply(server, query, clients, clients_count,
				 servers, servers_count, channels,
				 channels_count);
    silc_free(clients);
    silc_free(servers);
    silc_free(channels);
    return;
  }

  /* Now process all found information and if necessary do some more
     resolving. */
  switch (query->querycmd) {

  case SILC_COMMAND_WHOIS:
    for (i = 0; i < clients_count; i++) {
      client_entry = clients[i];

      /* Check if cannot query this anyway, so take next one */
      if (!client_entry ||
	  !(client_entry->data.status & SILC_IDLIST_STATUS_REGISTERED))
	continue;

      /* If Requested Attributes is set then we always resolve the client
	 information, if not then check whether the entry is complete or not
	 and decide whether we need to resolve or not. */
      if (!query->attrs) {

	/* Even if nickname and stuff are present, we may need to resolve
	   the entry */
	if (client_entry->nickname && client_entry->username &&
	    client_entry->userinfo) {
	  /* Check if cannot query this anyway, so take next one */
	  if (!client_entry->router)
	    continue;

	  /* If we are router, client is local to us, or client is on channel
	     we do not need to resolve the client information. */
	  if (server->server_type != SILC_SERVER || SILC_IS_LOCAL(client_entry)
	      || silc_hash_table_count(client_entry->channels) ||
	      query->resolved)
	    continue;
	}
      }

      /* When requested attributes is present and local client is detached
	 we cannot send the command to the client, we'll reply on behalf of
	 the client instead. */
      if (query->attrs && SILC_IS_LOCAL(client_entry) &&
	  (client_entry->mode & SILC_UMODE_DETACHED ||
	   client_entry->data.status & SILC_IDLIST_STATUS_NOATTR))
	continue;

      /* Resolve the detailed client information. If client is local we
	 know that attributes were present and we will resolve directly
	 from the client. Otherwise resolve from client's owner. */
      silc_server_query_resolve(server, query,
				(SILC_IS_LOCAL(client_entry) ?
				 client_entry->connection :
				 client_entry->router->connection),
				client_entry);
    }
    break;

  case SILC_COMMAND_WHOWAS:
    for (i = 0; i < clients_count; i++) {
      client_entry = clients[i];

      /* Check if cannot query this anyway, so take next one */
      if (!client_entry || !client_entry->router ||
	  client_entry->data.status & SILC_IDLIST_STATUS_REGISTERED)
	continue;

      /* If both nickname and username are present no resolving is needed */
      if (client_entry->nickname && client_entry->username)
	continue;

      /* Resolve the detailed client information */
      silc_server_query_resolve(server, query,
				client_entry->router->connection,
				client_entry);
    }
    break;

  case SILC_COMMAND_IDENTIFY:
    for (i = 0; i < clients_count; i++) {
      client_entry = clients[i];

      /* Check if cannot query this anyway, so take next one */
      if (!client_entry || !client_entry->router ||
	  !(client_entry->data.status & SILC_IDLIST_STATUS_REGISTERED))
	continue;

      /* Even if nickname is present, we may need to resolve the entry */
      if (client_entry->nickname) {

	/* If we are router, client is local to us, or client is on channel
	   we do not need to resolve the client information. */
	if (server->server_type != SILC_SERVER || SILC_IS_LOCAL(client_entry)
	    || silc_hash_table_count(client_entry->channels) ||
	    query->resolved)
	  continue;
      }

      /* Resolve the detailed client information */
      silc_server_query_resolve(server, query,
				client_entry->router->connection,
				client_entry);
    }
    break;
  }

  if (!query->queries_count)
    /* If we didn't have to do any resolving, continue with sending the
       command reply to the original sender. */
    silc_server_query_send_reply(server, query, clients, clients_count,
				 servers, servers_count, channels,
				 channels_count);
  else
    /* Now actually send the resolvings we gathered earlier */
    silc_server_query_resolve(server, query, NULL, NULL);

  silc_free(clients);
  silc_free(servers);
  silc_free(channels);
}

/* Resolve the detailed information for the `client_entry'.  Only client
   information needs to be resolved for being incomplete.  Each incomplete
   client entry calls this function to do the resolving. */

void silc_server_query_resolve(SilcServer server, SilcServerQuery query,
			       SilcSocketConnection sock,
			       SilcClientEntry client_entry)
{
  SilcServerCommandContext cmd = query->cmd;
  SilcServerQueryList r = NULL;
  SilcBuffer idp;
  unsigned char *tmp;
  SilcUInt32 len;
  SilcUInt16 ident;
  int i;

  if (!sock && client_entry)
    return;

  /* If arguments are NULL we will now actually send the resolvings
     that earlier has been gathered by calling this function. */
  if (!sock && !client_entry) {
    SilcBuffer res_cmd;

    SILC_LOG_DEBUG(("Sending the resolvings"));

    /* WHOWAS resolving has been done at the same time this function
       was called to add the resolving for WHOWAS, so just return. */
    if (query->querycmd == SILC_COMMAND_WHOWAS)
      return;

    for (i = 0; i < query->querylist_count; i++) {
      r = &query->querylist[i];

      /* Send WHOIS command */
      res_cmd = silc_command_payload_encode(SILC_COMMAND_WHOIS,
					    r->argc, r->arg, r->arg_lens,
					    r->arg_types, r->ident);
      silc_server_packet_send(server, r->sock, SILC_PACKET_COMMAND, 0,
			      res_cmd->data, res_cmd->len, FALSE);
      silc_buffer_free(res_cmd);

      /* Reprocess this packet after received reply */
      if (silc_server_command_pending_timed(server, SILC_COMMAND_WHOIS,
					    r->ident,
					    silc_server_query_resolve_reply,
					    query, r->timeout))
	query->queries_left++;
    }

    /* Cleanup this temporary context */
    for (i = 0; i < query->querylist_count; i++) {
      int k;
      for (k = 0; k < query->querylist[i].argc; k++)
	silc_free(query->querylist[i].arg[k]);
      silc_free(query->querylist[i].arg);
      silc_free(query->querylist[i].arg_lens);
      silc_free(query->querylist[i].arg_types);
    }
    silc_free(query->querylist);
    query->querylist = NULL;
    query->querylist_count = 0;
    return;
  }

  SILC_LOG_DEBUG(("Resolving client information"));

  if (client_entry->data.status & SILC_IDLIST_STATUS_RESOLVING) {
    /* The entry is being resolved by some other external query already.
       Attach to that query instead of resolving again. */
    ident = client_entry->resolve_cmd_ident;
    if (silc_server_command_pending(server, SILC_COMMAND_NONE, ident,
				    silc_server_query_resolve_reply, query))
      query->queries_left++;
  } else {
    /* This entry will be resolved */
    ident = ++server->cmd_ident;

    switch (query->querycmd) {

    case SILC_COMMAND_WHOIS:
    case SILC_COMMAND_IDENTIFY:
      /* Take existing query context if exist for this connection */
      for (i = 0; i < query->queries_count; i++)
	if (query->querylist[i].sock == sock) {
	  r = &query->querylist[i];
	  break;
	}

      if (!r) {
	/* Allocate new temp query list context */
	query->querylist = silc_realloc(query->querylist,
					sizeof(*query->querylist) * 
					(query->querylist_count + 1));
	r = &query->querylist[query->querylist_count];
	query->querylist_count++;
	memset(r, 0, sizeof(*r));
	r->sock = sock;
	r->ident = ident;
	if (SILC_IS_LOCAL(client_entry))
	  r->timeout = 3;
      }

      /* If Requested Attributes were present put them to this resolving */
      if (query->attrs && query->querycmd == SILC_COMMAND_WHOIS) {
	len = r->argc + 1;
	r->arg = silc_realloc(r->arg, sizeof(*r->arg) * len);
	r->arg_lens = silc_realloc(r->arg_lens, sizeof(*r->arg_lens) * len);
	r->arg_types = silc_realloc(r->arg_types, sizeof(*r->arg_types) * len);

	tmp = silc_argument_get_arg_type(cmd->args, 3, &len);
	if (tmp)
	  r->arg[r->argc] = silc_memdup(tmp, len);
	r->arg_lens[r->argc] = len;
	r->arg_types[r->argc] = 3;
	r->argc++;
      }

      len = r->argc + 1;
      r->arg = silc_realloc(r->arg, sizeof(*r->arg) * len);
      r->arg_lens = silc_realloc(r->arg_lens, sizeof(*r->arg_lens) * len);
      r->arg_types = silc_realloc(r->arg_types, sizeof(*r->arg_types) * len);

      /* Add the client entry to be resolved */
      idp = silc_id_payload_encode(client_entry->id, SILC_ID_CLIENT);
      r->arg[r->argc] = silc_memdup(idp->data, idp->len);
      r->arg_lens[r->argc] = idp->len;
      r->arg_types[r->argc] = r->argc + 4;
      r->argc++;
      silc_buffer_free(idp);

      break;

    case SILC_COMMAND_WHOWAS:
      /* We must send WHOWAS command since it's the only the way of
	 resolving clients that are not present in the network anymore. */
      silc_server_send_command(server, sock, query->querycmd, ident, 1,
			       1, query->nickname, strlen(query->nickname));
      if (silc_server_command_pending(server, query->querycmd, ident,
				      silc_server_query_resolve_reply, query))
	query->queries_left++;
      break;
    }
  }

  /* Mark the entry as being resolved */
  client_entry->data.status |= SILC_IDLIST_STATUS_RESOLVING;
  client_entry->data.status &= ~SILC_IDLIST_STATUS_RESOLVED;
  client_entry->resolve_cmd_ident = ident;

  /* Save the queried ID, which we will reprocess after we get this and
     all other queries back. */
  query->queries = silc_realloc(query->queries, sizeof(*query->queries) *
				(query->queries_count + 1));
  if (query->queries) {
    i = query->queries_count;
    query->queries[i].id = silc_id_dup(client_entry->id, SILC_ID_CLIENT);
    query->queries[i].id_type = SILC_ID_CLIENT;
    query->queries[i].ident = ident;
    query->queries_count++;
  }
}

/* Reply callback called after one resolving has been completed.  If
   all resolvings has been received then we will continue with sending
   the command reply to the original sender of the query. */

void silc_server_query_resolve_reply(void *context, void *reply)
{
  SilcServerQuery query = context;
  SilcServer server = query->cmd->server;
  SilcServerCommandReplyContext cmdr = reply;
  SilcUInt16 ident = cmdr->ident;
  SilcStatus error = SILC_STATUS_OK;
  SilcServerQueryID id = NULL;
  SilcClientEntry client_entry;
  int i;

  /* One less query left */
  query->queries_left--;

  silc_command_get_status(cmdr->payload, NULL, &error);
  SILC_LOG_DEBUG(("Received reply to resolving (%d left) (status=%d)",
		  query->queries_left, error));

  /* If no error then skip to other stuff */
  if (error == SILC_STATUS_OK)
    goto out;

  /* Error occurred during resolving */

  /* Find the resolved client ID */
  for (i = 0; i < query->queries_count; i++) {
    if (query->queries[i].ident != ident)
      continue;

    id = &query->queries[i];

    if (error == SILC_STATUS_ERR_TIMEDOUT) {

      /* If timeout occurred for local entry when resolving attributes
	 mark that this client doesn't support attributes in WHOIS. This
	 assures we won't send the request again to the client. */
      if (query->querycmd == SILC_COMMAND_WHOIS && query->attrs) {
	client_entry = silc_idlist_find_client_by_id(server->local_list,
						     id->id, TRUE, NULL);
	SILC_LOG_DEBUG(("Client %s does not support Requested Attributes",
			silc_id_render(id->id, SILC_ID_CLIENT)));
	if (client_entry && SILC_IS_LOCAL(client_entry)) {
	  client_entry->data.status |= SILC_IDLIST_STATUS_NOATTR;
	  client_entry->data.status &= ~SILC_IDLIST_STATUS_RESOLVING;
          continue;
	}
      }

      /* Remove the RESOLVING status from the client entry */
      if (query->querycmd != SILC_COMMAND_WHOWAS) {
	client_entry = silc_idlist_find_client_by_id(server->local_list,
						     id->id, TRUE, NULL);
	if (!client_entry)
	  client_entry = silc_idlist_find_client_by_id(server->global_list,
						       id->id, TRUE, NULL);
	if (client_entry)
	  client_entry->data.status &= ~SILC_IDLIST_STATUS_RESOLVING;
      }
    }
  }

 out:

  /* If there are queries left then wait for them */
  if (query->queries_left)
    return;

  SILC_LOG_DEBUG(("Reprocess the query"));

  /* We have received all queries.  Now re-search all information required
     to complete this query.  Reason we cannot save the values found in
     the first search is that SilcClientEntry, SilcServerEntry and
     SilcChannelEntry pointers may become invalid while we were waiting
     for these resolvings. */
  silc_server_query_process(server, query, FALSE);
}

/* Send the reply to the original query.  If arguments are NULL then this
   sends only the errors that has occurred during the processing of the
   query.  This sends the errors always after sending all the found
   information.  The query is over after this function returns and the
   `query' will become invalid.  This is called only after all informations
   has been resolved.  This means that if something is not found or is
   incomplete in this function we were unable to resolve the information
   or it does not exist at all. */

void silc_server_query_send_reply(SilcServer server,
				  SilcServerQuery query,
				  SilcClientEntry *clients,
				  SilcUInt32 clients_count,
				  SilcServerEntry *servers,
				  SilcUInt32 servers_count,
				  SilcChannelEntry *channels,
				  SilcUInt32 channels_count)
{
  SilcServerCommandContext cmd = query->cmd;
  SilcUInt16 ident = silc_command_get_ident(cmd->payload);
  SilcStatus status;
  unsigned char *tmp;
  SilcUInt32 len;
  SilcBuffer idp;
  int i, k, valid_count;
  char nh[256], uh[256];
  bool sent_reply = FALSE;

  SILC_LOG_DEBUG(("Sending reply to query"));

  status = SILC_STATUS_OK;

  /* Send clients */
  if (clients_count) {
    SilcClientEntry entry;
    SilcSocketConnection hsock;

    /* Mark all invalid entries */
    for (i = 0, valid_count = 0; i < clients_count; i++) {
      entry = clients[i];
      switch (query->querycmd) {
      case SILC_COMMAND_WHOIS:
	if (!entry->nickname || !entry->username || !entry->userinfo ||
	    !(entry->data.status & SILC_IDLIST_STATUS_REGISTERED)) {
	  /* When querying by ID, every "unfound" entry must cause error */
	  if (query->ids)
	    silc_server_query_add_error_id(server, query,
					   SILC_STATUS_ERR_TIMEDOUT,
					   entry->id, SILC_ID_CLIENT);
	  clients[i] = NULL;
	  continue;
	}
	break;

      case SILC_COMMAND_IDENTIFY:
	if (!entry->nickname ||
	    !(entry->data.status & SILC_IDLIST_STATUS_REGISTERED)) {
	  /* When querying by ID, every "unfound" entry must cause error */
	  if (query->ids)
	    silc_server_query_add_error_id(server, query,
					   SILC_STATUS_ERR_TIMEDOUT,
					   entry->id, SILC_ID_CLIENT);
	  clients[i] = NULL;
	  continue;
	}
	break;

      case SILC_COMMAND_WHOWAS:
	if (!entry->nickname || !entry->username ||
	    entry->data.status & SILC_IDLIST_STATUS_REGISTERED) {
	  clients[i] = NULL;
	  continue;
	}
	break;
      }
      valid_count++;
    }

    /* Start processing found clients */
    status = SILC_STATUS_OK;
    if (valid_count > 1)
      status = SILC_STATUS_LIST_START;

    /* Now do the sending of valid entries */
    k = 0;
    for (i = 0; i < clients_count && valid_count; i++) {
      entry = clients[i];
      if (!entry)
	continue;

      if (k >= 1)
	status = SILC_STATUS_LIST_ITEM;
      if (valid_count > 1 && k == valid_count - 1 
	  && !servers_count && !channels_count && !query->errors_count)
	status = SILC_STATUS_LIST_END;
      if (query->reply_count && k - 1 == query->reply_count)
	status = SILC_STATUS_LIST_END;

      SILC_LOG_DEBUG(("%s: client %s",
		      (status == SILC_STATUS_OK ?         "   OK" :
		       status == SILC_STATUS_LIST_START ? "START" :
		       status == SILC_STATUS_LIST_ITEM  ? " ITEM" :
		       status == SILC_STATUS_LIST_END  ?  "  END" :
		       "      : "), entry->nickname));

      idp = silc_id_payload_encode(entry->id, SILC_ID_CLIENT);
      memset(uh, 0, sizeof(uh));
      memset(nh, 0, sizeof(nh));

      silc_strncat(nh, sizeof(nh), entry->nickname, strlen(entry->nickname));
      if (!strchr(entry->nickname, '@')) {
	silc_strncat(nh, sizeof(nh), "@", 1);
	if (entry->servername) {
	  silc_strncat(nh, sizeof(nh), entry->servername,
		       strlen(entry->servername));
	} else {
	  len = entry->router ? strlen(entry->router->server_name) :
	    strlen(server->server_name);
	  silc_strncat(nh, sizeof(nh), entry->router ?
		       entry->router->server_name :
		       server->server_name, len);
	}
      }
      
      switch (query->querycmd) {
	
      case SILC_COMMAND_WHOIS:
	{
	  unsigned char idle[4], mode[4];
	  unsigned char *fingerprint, fempty[20], *attrs = NULL;
	  SilcBuffer channels, umode_list = NULL;

	  memset(fempty, 0, sizeof(fempty));
	  silc_strncat(uh, sizeof(uh), entry->username,
		       strlen(entry->username));
	  if (!strchr(entry->username, '@') && entry->connection) {
	    hsock = entry->connection;
	    silc_strncat(uh, sizeof(uh), "@", 1);
	    len = strlen(hsock->hostname);
	    silc_strncat(uh, sizeof(uh), hsock->hostname, len);
	  }

	  if (cmd->sock->type == SILC_SOCKET_TYPE_CLIENT)
	    channels =
	      silc_server_get_client_channel_list(server, entry, FALSE, 
						  FALSE, &umode_list);
	  else
	    channels =
	      silc_server_get_client_channel_list(server, entry, TRUE, 
						  TRUE, &umode_list);

	  if (memcmp(entry->data.fingerprint, fempty, sizeof(fempty)))
	    fingerprint = entry->data.fingerprint;
	  else
	    fingerprint = NULL;

	  SILC_PUT32_MSB(entry->mode, mode);
	  if (entry->connection)
	    SILC_PUT32_MSB((time(NULL) - entry->data.last_receive), idle);

	  /* If Requested Attribute were present, and we do not have the
	     attributes we will reply to them on behalf of the client. */
	  len = 0;
	  if (query->attrs) {
	    if (!entry->attrs) {
	      attrs = silc_server_query_reply_attrs(server, query, &len);
	    } else {
	      attrs = entry->attrs;
	      len = entry->attrs_len;
	    }
	  }

	  /* Send command reply */
	  silc_server_send_command_reply(server, cmd->sock, query->querycmd,
					 status, 0, ident, 10,
					 2, idp->data, idp->len,
					 3, nh, strlen(nh),
					 4, uh, strlen(uh),
					 5, entry->userinfo, 
					 strlen(entry->userinfo),
					 6, channels ? channels->data : NULL,
					 channels ? channels->len : 0,
					 7, mode, 4,
					 8, idle, 4,
					 9, fingerprint,
					 fingerprint ? 20 : 0,
					 10, umode_list ? umode_list->data :
					 NULL, umode_list ? umode_list->len :
					 0, 11, attrs, len);

	  sent_reply = TRUE;

	  /* For now we will delete Requested Attributes */
	  silc_free(entry->attrs);
	  entry->attrs = NULL;

	  if (channels)
	    silc_buffer_free(channels);
	  if (umode_list) {
	    silc_buffer_free(umode_list);
	    umode_list = NULL;
	  }
	}
	break;

      case SILC_COMMAND_IDENTIFY:
	if (!entry->username) {
	  silc_server_send_command_reply(server, cmd->sock, query->querycmd,
					 status, 0, ident, 2,
					 2, idp->data, idp->len,
					 3, nh, strlen(nh));
	  sent_reply = TRUE;
	} else {
	  silc_strncat(uh, sizeof(uh), entry->username,
		       strlen(entry->username));
	  if (!strchr(entry->username, '@') && entry->connection) {
	    hsock = entry->connection;
	    silc_strncat(uh, sizeof(uh), "@", 1);
	    len = strlen(hsock->hostname);
	    silc_strncat(uh, sizeof(uh), hsock->hostname, len);
	  }

	  silc_server_send_command_reply(server, cmd->sock, query->querycmd,
					 status, 0, ident, 3,
					 2, idp->data, idp->len,
					 3, nh, strlen(nh),
					 4, uh, strlen(uh));
	  sent_reply = TRUE;
	}
	break;

      case SILC_COMMAND_WHOWAS:
	silc_strncat(uh, sizeof(uh), entry->username, strlen(entry->username));
	if (!strchr(entry->username, '@'))
	  silc_strncat(uh, sizeof(uh), "@*private*", 10);

	/* Send command reply */
	silc_server_send_command_reply(server, cmd->sock, query->querycmd,
				       status, 0, ident, 4,
				       2, idp->data, idp->len,
				       3, nh, strlen(nh),
				       4, uh, strlen(uh),
				       5, entry->userinfo, 
				       entry->userinfo ? 
				       strlen(entry->userinfo) : 0);
	sent_reply = TRUE;
	break;
      }

      silc_buffer_free(idp);

      if (status == SILC_STATUS_LIST_END)
	break;
      k++;
    }

    if (k == 0) {
      /* Not one valid entry was found, send error.  If nickname was used
	 in query send error based on that, otherwise the query->errors
	 already includes proper errors. */
      if (query->nickname)
	silc_server_query_add_error(server, query, TRUE, 1,
				    SILC_STATUS_ERR_NO_SUCH_NICK);
    }
  }

  /* Send servers */
  if (query->querycmd == SILC_COMMAND_IDENTIFY && servers_count) {
    SilcServerEntry entry;

    if (status == SILC_STATUS_OK && servers_count > 1)
      status = SILC_STATUS_LIST_START;

    k = 0;
    for (i = 0; i < servers_count; i++) {
      entry = servers[i];
      
      if (k >= 1)
	status = SILC_STATUS_LIST_ITEM;
      if (servers_count > 1 && k == servers_count - 1 && !channels_count &&
	  !query->errors_count)
	status = SILC_STATUS_LIST_END;
      if (query->reply_count && k - 1 == query->reply_count)
	status = SILC_STATUS_LIST_END;
      
      SILC_LOG_DEBUG(("%s: server %s",
		      (status == SILC_STATUS_OK ?         "   OK" :
		       status == SILC_STATUS_LIST_START ? "START" :
		       status == SILC_STATUS_LIST_ITEM  ? " ITEM" :
		       status == SILC_STATUS_LIST_END  ?  "  END" :
		       "      : "),
		      entry->server_name ? entry->server_name : ""));

      /* Send command reply */
      idp = silc_id_payload_encode(entry->id, SILC_ID_SERVER);
      silc_server_send_command_reply(server, cmd->sock, SILC_COMMAND_IDENTIFY,
				     status, 0, ident, 2,
				     2, idp->data, idp->len, 
				     3, entry->server_name, 
				     entry->server_name ? 
				     strlen(entry->server_name) : 0);
      silc_buffer_free(idp);
      sent_reply = TRUE;
      
      if (status == SILC_STATUS_LIST_END)
	break;
      k++;
    }
  }

  /* Send channels */
  if (query->querycmd == SILC_COMMAND_IDENTIFY && channels_count) {
    SilcChannelEntry entry;

    if (status == SILC_STATUS_OK && channels_count > 1)
      status = SILC_STATUS_LIST_START;

    k = 0;
    for (i = 0; i < channels_count; i++) {
      entry = channels[i];
      
      if (k >= 1)
	status = SILC_STATUS_LIST_ITEM;
      if (channels_count > 1 && k == channels_count - 1 &&
	  !query->errors_count)
	status = SILC_STATUS_LIST_END;
      if (query->reply_count && k - 1 == query->reply_count)
	status = SILC_STATUS_LIST_END;
      
      SILC_LOG_DEBUG(("%s: channel %s",
		      (status == SILC_STATUS_OK ?         "   OK" :
		       status == SILC_STATUS_LIST_START ? "START" :
		       status == SILC_STATUS_LIST_ITEM  ? " ITEM" :
		       status == SILC_STATUS_LIST_END  ?  "  END" :
		       "      : "),
		      entry->channel_name ? entry->channel_name : ""));

      /* Send command reply */
      idp = silc_id_payload_encode(entry->id, SILC_ID_CHANNEL);
      silc_server_send_command_reply(server, cmd->sock, SILC_COMMAND_IDENTIFY,
				     status, 0, ident, 2,
				     2, idp->data, idp->len, 
				     3, entry->channel_name, 
				     entry->channel_name ? 
				     strlen(entry->channel_name) : 0);
      silc_buffer_free(idp);
      sent_reply = TRUE;
      
      if (status == SILC_STATUS_LIST_END)
	break;
      k++;
    }
  }

  /* Send errors */
  if (query->errors_count) {
    int type;

    if (status == SILC_STATUS_OK && query->errors_count > 1)
      status = SILC_STATUS_LIST_START;

    k = 0;
    for (i = 0; i < query->errors_count; i++) {
      idp = NULL;

      /* Take error argument */
      if (query->errors[i].from_cmd) {
	len = 0;
	tmp = silc_argument_get_arg_type(cmd->args,
					 query->errors[i].index, &len);
	if (query->errors[i].index == 1)
	  type = 3;		    /* Nickname */
	else
	  type = 2;		    /* ID */
      } else if (!query->errors[i].id) {
	idp =
	  silc_id_payload_encode(query->ids[query->errors[i].index].id,
				 query->ids[query->errors[k].index].id_type);
	tmp = idp->data;
	len = idp->len;
	type = 2;
      } else {
	idp = silc_id_payload_encode(query->errors[i].id,
				     query->errors[k].id_type);
	tmp = idp->data;
	len = idp->len;
	type = 2;
      }

      if (k >= 1)
	status = SILC_STATUS_LIST_ITEM;
      if (query->errors_count > 1 && k == query->errors_count - 1)
	status = SILC_STATUS_LIST_END;
      if (query->reply_count && k - 1 == query->reply_count)
	status = SILC_STATUS_LIST_END;

      SILC_LOG_DEBUG(("%s: ERROR: %s (%d)",
		      (status == SILC_STATUS_OK ?         "   OK" :
		       status == SILC_STATUS_LIST_START ? "START" :
		       status == SILC_STATUS_LIST_ITEM  ? " ITEM" :
		       status == SILC_STATUS_LIST_END  ?  "  END" :
		       "      : "), 
		      silc_get_status_message(query->errors[i].error),
		      query->errors[i].error));

      /* Send error */
      silc_server_send_command_reply(server, cmd->sock, query->querycmd,
				     (status == SILC_STATUS_OK ?
				      query->errors[i].error : status),
				     (status == SILC_STATUS_OK ?
				      0 : query->errors[i].error), ident, 1,
				     type, tmp, len);
      silc_buffer_free(idp);
      sent_reply = TRUE;

      if (status == SILC_STATUS_LIST_END)
	break;
      k++;
    }
  }

  if (!sent_reply)
    SILC_LOG_ERROR(("BUG: Query did not send anything"));

  /* Cleanup */
  silc_server_query_free(query);
}

/* This routine is used to reply to Requested Attributes in WHOIS on behalf
   of the client since we were unable to resolve them from the client.
   Either client does not support Requested Attributes or isn't replying
   to them like it should. */

unsigned char *silc_server_query_reply_attrs(SilcServer server,
					     SilcServerQuery query,
					     SilcUInt32 *attrs_len)
{
  unsigned char *attrs = NULL;
  SilcUInt32 len = 0;

  SILC_LOG_DEBUG(("Constructing Requested Attributes"));

  if (attrs_len)
    *attrs_len = len;

  return attrs;
}

/* Find client by the Client ID indicated by the `client_id', and if not
   found then query it by using WHOIS command.  The client information
   is also resolved if the cached information is incomplete or if the
   `always_resolve' is set to TRUE.  The indication whether requested
   client was being resolved is saved into `resolved'.  If the client
   is not being resolved its entry is returned by this function.  NULL
   is returned if client is resolved. */

SilcClientEntry silc_server_query_client(SilcServer server,
					 const SilcClientID *client_id,
					 bool always_resolve,
					 bool *resolved)
{
  SilcClientEntry client;

  SILC_LOG_DEBUG(("Resolving client by client ID"));

  if (resolved)
    *resolved = FALSE;

  client = silc_idlist_find_client_by_id(server->local_list,
					 (SilcClientID *)client_id,
					 TRUE, NULL);
  if (!client) {
    client = silc_idlist_find_client_by_id(server->global_list,
					   (SilcClientID *)client_id,
					   TRUE, NULL);
    if (!client && server->server_type == SILC_ROUTER)
      return NULL;
  }

  if (!client && server->standalone)
    return NULL;

  if (!client || !client->nickname || !client->username ||
      always_resolve) {
    SilcBuffer buffer, idp;

    if (client) {
      client->data.status |= SILC_IDLIST_STATUS_RESOLVING;
      client->data.status &= ~SILC_IDLIST_STATUS_RESOLVED;
      client->resolve_cmd_ident = ++server->cmd_ident;
    }

    idp = silc_id_payload_encode(client_id, SILC_ID_CLIENT);
    buffer = silc_command_payload_encode_va(SILC_COMMAND_WHOIS,
					    server->cmd_ident, 1,
					    4, idp->data, idp->len);
    silc_server_packet_send(server, client ? client->router->connection :
			    SILC_PRIMARY_ROUTE(server),
			    SILC_PACKET_COMMAND, 0,
			    buffer->data, buffer->len, FALSE);
    silc_buffer_free(idp);
    silc_buffer_free(buffer);

    if (resolved)
      *resolved = TRUE;

    return NULL;
  }

  return client;
}