/**
 * (n+1)Sec Multiparty Off-the-Record Messaging library
 * Copyright (C) 2016, eQualit.ie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 3 of the GNU Lesser General
 * Public License as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "channel.h"
#include "room.h"

namespace np1sec
{

Channel::Channel(Room* room):
	m_room(room),
	m_ephemeral_private_key(PrivateKey::generate()),
	m_interface(nullptr),
	m_joined(true),
	m_active(false),
	m_authorized(true),
	m_channel_status_hash(crypto::nonce_hash()),
	m_encrypted_chat(this)
{
	Participant self;
	self.username = m_room->username();
	self.long_term_public_key = m_room->long_term_public_key();
	self.ephemeral_public_key = ephemeral_public_key();
	self.authorization_nonce = m_channel_status_hash;
	self.authorized = true;
	self.authentication_status = AuthenticationStatus::Authenticated;
	m_participants[self.username] = self;
	
	m_encrypted_chat.create_solo_session();
}

Channel::Channel(Room* room, const ChannelStatusMessage& channel_status, const Message& encoded_message):
	m_room(room),
	m_ephemeral_private_key(PrivateKey::generate()),
	m_interface(nullptr),
	m_joined(false),
	m_active(false),
	m_authorized(false),
	m_encrypted_chat(this),
	m_authentication_nonce(crypto::nonce_hash())
{
	/*
	 * The event queue in the channel_status message does not contain the
	 * event describing this status message, so we need to construct it.
	 */
	Event channel_status_event;
	channel_status_event.type = Message::Type::ChannelStatus;
	channel_status_event.channel_status.searcher_username = channel_status.searcher_username;
	channel_status_event.channel_status.searcher_nonce = channel_status.searcher_nonce;
	channel_status_event.channel_status.status_message_hash = crypto::hash(encoded_message.payload);
	
	for (const ChannelStatusMessage::Participant& p : channel_status.participants) {
		Participant participant;
		participant.username = p.username;
		participant.long_term_public_key = p.long_term_public_key;
		participant.ephemeral_public_key = p.ephemeral_public_key;
		participant.authorization_nonce = p.authorization_nonce;
		participant.authorized = true;
		participant.authentication_status = AuthenticationStatus::Unauthenticated;
		
		if (m_participants.count(participant.username)) {
			throw MessageFormatException();
		}
		
		m_participants[participant.username] = std::move(participant);
		
		channel_status_event.channel_status.remaining_users.insert(p.username);
		
		m_encrypted_chat.do_add_user(p.username, p.long_term_public_key);
	}
	
	for (const ChannelStatusMessage::UnauthorizedParticipant& p : channel_status.unauthorized_participants) {
		Participant participant;
		participant.username = p.username;
		participant.long_term_public_key = p.long_term_public_key;
		participant.ephemeral_public_key = p.ephemeral_public_key;
		participant.authorization_nonce = p.authorization_nonce;
		participant.authorized = false;
		participant.authentication_status = AuthenticationStatus::Unauthenticated;
		
		for (const std::string& peer : p.authorized_by) {
			if (m_participants.count(peer)) {
				participant.authorized_by.insert(peer);
			}
		}
		for (const std::string& peer : p.authorized_peers) {
			if (m_participants.count(peer)) {
				participant.authorized_peers.insert(peer);
			}
		}
		
		if (m_participants.count(participant.username)) {
			throw MessageFormatException();
		}
		
		m_participants[participant.username] = std::move(participant);
		
		channel_status_event.channel_status.remaining_users.insert(p.username);
	}
	
	m_channel_status_hash = channel_status.channel_status_hash;
	
	std::set<Hash> key_exchange_ids;
	std::set<Hash> key_exchange_event_ids;
	std::set<Hash> key_activation_event_ids;
	std::set<Hash> key_ids_seen;
	for (const KeyExchangeState& exchange : channel_status.key_exchanges) {
		if (key_exchange_ids.count(exchange.key_id)) {
			throw MessageFormatException();
		}
		
		m_encrypted_chat.unserialize_key_exchange(exchange);
		
		key_exchange_ids.insert(exchange.key_id);
	}
	
	for (const ChannelEvent& channel_event : channel_status.events) {
		Event event;
		event.type = channel_event.type;
		if (channel_event.type == Message::Type::ChannelStatus) {
			event.channel_status = ChannelStatusEvent::decode(channel_event, channel_status);
		} else if (channel_event.type == Message::Type::ConsistencyCheck) {
			event.consistency_check = ConsistencyCheckEvent::decode(channel_event, channel_status);
		} else if (
			   channel_event.type == Message::Type::KeyExchangePublicKey
			|| channel_event.type == Message::Type::KeyExchangeSecretShare
			|| channel_event.type == Message::Type::KeyExchangeAcceptance
			|| channel_event.type == Message::Type::KeyExchangeReveal
		) {
			KeyExchangeEvent e = KeyExchangeEvent::decode(channel_event, channel_status);
			event.key_event.key_id = e.key_id;
			if (e.cancelled) {
				if (key_exchange_ids.count(event.key_event.key_id)) {
					throw MessageFormatException();
				}
				event.key_event.remaining_users = e.remaining_users;
			} else {
				if (!key_exchange_ids.count(event.key_event.key_id)) {
					throw MessageFormatException();
				}
				if (key_exchange_event_ids.count(event.key_event.key_id)) {
					throw MessageFormatException();
				}
				key_exchange_event_ids.insert(event.key_event.key_id);
			}
		} else if (channel_event.type == Message::Type::KeyActivation) {
			event.key_event = KeyActivationEvent::decode(channel_event, channel_status);
			if (key_exchange_ids.count(event.key_event.key_id)) {
				throw MessageFormatException();
			}
			if (key_activation_event_ids.count(event.key_event.key_id)) {
				throw MessageFormatException();
			}
			key_activation_event_ids.insert(event.key_event.key_id);
		} else {
			throw MessageFormatException();
		}
		m_events.push_back(std::move(event));
	}
	
	/*
	 * Each key exchange key ID must appear as exactly one key-exchange event.
	 */
	if (key_exchange_ids.size() != key_exchange_event_ids.size()) {
		throw MessageFormatException();
	}
	
	m_events.push_back(std::move(channel_status_event));
}

Channel::Channel(Room* room, const ChannelAnnouncementMessage& channel_status, const std::string& sender):
	m_room(room),
	m_ephemeral_private_key(PrivateKey::generate()),
	m_interface(nullptr),
	m_joined(false),
	m_active(false),
	m_authorized(false),
	m_encrypted_chat(this),
	m_authentication_nonce(crypto::nonce_hash())
{
	m_channel_status_hash = channel_status.channel_status_hash;
	
	Participant participant;
	participant.username = sender;
	participant.long_term_public_key = channel_status.long_term_public_key;
	participant.ephemeral_public_key = channel_status.ephemeral_public_key;
	participant.authorization_nonce = channel_status.channel_status_hash;
	participant.authorized = true;
	participant.authentication_status = AuthenticationStatus::Unauthenticated;
	m_participants[participant.username] = std::move(participant);
	
	m_encrypted_chat.do_add_user(sender, channel_status.long_term_public_key);
}

void Channel::send_chat(const std::string& message)
{
	m_encrypted_chat.send_message(message);
}

void Channel::announce()
{
	ChannelAnnouncementMessage message;
	message.long_term_public_key = m_room->long_term_public_key();
	message.ephemeral_public_key = ephemeral_public_key();
	message.channel_status_hash = m_channel_status_hash;
	send_message(message.encode());
}

void Channel::confirm_participant(const std::string& username)
{
	if (!m_participants.count(username)) {
		return;
	}
	
	Participant& participant = m_participants[username];
	if (participant.authentication_status == AuthenticationStatus::Unauthenticated) {
		participant.authentication_status = AuthenticationStatus::AuthenticatingWithNonce;
		
		AuthenticationRequestMessage request;
		request.sender_long_term_public_key = m_room->long_term_public_key();
		request.sender_ephemeral_public_key = ephemeral_public_key();
		request.peer_username = participant.username;
		request.peer_long_term_public_key = participant.long_term_public_key;
		request.peer_ephemeral_public_key = participant.ephemeral_public_key;
		request.nonce = m_authentication_nonce;
		send_message(request.encode());
	}
}

void Channel::join()
{
	JoinRequestMessage message;
	message.long_term_public_key = m_room->long_term_public_key();
	message.ephemeral_public_key = ephemeral_public_key();
	
	for (const auto& i : m_participants) {
		message.peer_usernames.push_back(i.first);
	}
	
	send_message(message.encode());
}

void Channel::activate()
{
	m_active = true;
	
	set_channel_status_timer();
}

void Channel::authorize(const std::string& username)
{
	if (!m_participants.count(username)) {
		return;
	}
	
	if (username == m_room->username()) {
		return;
	}
	
	Participant& participant = m_participants[username];
	Participant& self = m_participants[m_room->username()];
	
	if (self.authorized) {
		if (participant.authorized) {
			return;
		}
		
		if (participant.authorized_by.count(m_room->username())) {
			return;
		}
	} else {
		if (!participant.authorized) {
			return;
		}
		
		if (self.authorized_peers.count(username)) {
			return;
		}
	}
	
	UnsignedAuthorizationMessage message;
	message.username = participant.username;
	message.long_term_public_key = participant.long_term_public_key;
	message.ephemeral_public_key = participant.ephemeral_public_key;
	message.authorization_nonce = participant.authorization_nonce;
	send_message(AuthorizationMessage::sign(message, m_ephemeral_private_key));
}

void Channel::message_received(const std::string& sender, const Message& np1sec_message)
{
	hash_message(sender, np1sec_message);
	
	if (np1sec_message.type == Message::Type::ChannelSearch) {
		ChannelSearchMessage message;
		try {
			message = ChannelSearchMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		Event consistency_check_event;
		consistency_check_event.type = Message::Type::ConsistencyCheck;
		consistency_check_event.consistency_check.channel_status_hash = m_channel_status_hash;
		for (const auto& i : m_participants) {
			consistency_check_event.consistency_check.remaining_users.insert(i.second.username);
		}
		m_events.push_back(std::move(consistency_check_event));
		
		if (m_active) {
			UnsignedConsistencyCheckMessage consistency_check_message;
			consistency_check_message.channel_status_hash = m_channel_status_hash;
			send_message(ConsistencyCheckMessage::sign(consistency_check_message, m_ephemeral_private_key));
		}
		
		Message reply = channel_status(sender, message.nonce);
		
		Event reply_event;
		reply_event.type = Message::Type::ChannelStatus;
		reply_event.channel_status.searcher_username = sender;
		reply_event.channel_status.searcher_nonce = message.nonce;
		reply_event.channel_status.status_message_hash = crypto::hash(reply.payload);
		for (const auto& i : m_participants) {
			reply_event.channel_status.remaining_users.insert(i.second.username);
		}
		m_events.push_back(std::move(reply_event));
		
		if (m_active) {
			send_message(reply);
		}
	} else if (np1sec_message.type == Message::Type::ChannelStatus) {
		ChannelStatusMessage message;
		try {
			message = ChannelStatusMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::ChannelStatus
			&& first_event->channel_status.searcher_username == message.searcher_username
			&& first_event->channel_status.searcher_nonce == message.searcher_nonce
			&& first_event->channel_status.status_message_hash == crypto::hash(np1sec_message.payload)
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->channel_status.remaining_users.erase(sender);
		if (first_event->channel_status.remaining_users.empty()) {
			m_events.erase(first_event);
		}
	} else if (np1sec_message.type == Message::Type::ChannelAnnouncement) {
		ChannelAnnouncementMessage message;
		try {
			message = ChannelAnnouncementMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		if (m_participants.count(sender)) {
			remove_user(sender);
		}
	} else if (np1sec_message.type == Message::Type::JoinRequest) {
		JoinRequestMessage message;
		try {
			message = JoinRequestMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		remove_user(sender);
		
		bool ours = false;
		for (const std::string& username : message.peer_usernames) {
			if (m_participants.count(username)) {
				ours = true;
				break;
			}
		}
		if (!ours) {
			return;
		}
		
		Participant participant;
		participant.username = sender;
		participant.long_term_public_key = message.long_term_public_key;
		participant.ephemeral_public_key = message.ephemeral_public_key;
		participant.authorization_nonce = m_channel_status_hash;
		participant.authorized = false;
		m_participants[sender] = std::move(participant);
		
		if (sender == m_room->username()) {
			m_participants[sender].authentication_status = AuthenticationStatus::Authenticated;
		} else if (!m_active) {
			m_participants[sender].authentication_status = AuthenticationStatus::AuthenticatingWithNonce;
			
			AuthenticationRequestMessage request;
			request.sender_long_term_public_key = m_room->long_term_public_key();
			request.sender_ephemeral_public_key = ephemeral_public_key();
			request.peer_username = sender;
			request.peer_long_term_public_key = message.long_term_public_key;
			request.peer_ephemeral_public_key = message.ephemeral_public_key;
			request.nonce = m_authentication_nonce;
			send_message(request.encode());
		} else {
			m_participants[sender].authentication_status = AuthenticationStatus::Authenticating;
		}
		
		if (m_interface) {
			m_interface->user_joined(sender);
		}
		
		if (sender == m_room->username()) {
			self_joined();
		}
	} else if (np1sec_message.type == Message::Type::AuthenticationRequest) {
		AuthenticationRequestMessage message;
		try {
			message = AuthenticationRequestMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		if (!m_active) {
			return;
		}
		
		if (
			   message.peer_username == m_room->username()
			&& message.peer_long_term_public_key == m_room->long_term_public_key()
			&& message.peer_ephemeral_public_key == ephemeral_public_key()
		) {
			authenticate_to(sender, message.sender_long_term_public_key, message.sender_ephemeral_public_key, message.nonce);
		}
	} else if (np1sec_message.type == Message::Type::Authentication) {
		AuthenticationMessage message;
		try {
			message = AuthenticationMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		if (!(
			   message.peer_username == m_room->username()
			&& message.peer_long_term_public_key == m_room->long_term_public_key()
			&& message.peer_ephemeral_public_key == ephemeral_public_key()
		)) {
			return;
		}
		
		if (!m_participants.count(sender)) {
			return;
		}
		
		Participant& participant = m_participants[sender];
		if (!(
			   message.sender_long_term_public_key == participant.long_term_public_key
			&& message.sender_ephemeral_public_key == participant.ephemeral_public_key
		)) {
			return;
		}
		
		if (participant.authentication_status == AuthenticationStatus::Authenticating) {
			if (message.nonce != participant.authorization_nonce) {
				return;
			}
		} else if (participant.authentication_status == AuthenticationStatus::AuthenticatingWithNonce) {
			if (message.nonce != participant.authorization_nonce && message.nonce != m_authentication_nonce) {
				return;
			}
		} else {
			return;
		}
		
		Hash correct_token = authentication_token(sender, participant.long_term_public_key, participant.ephemeral_public_key, message.nonce, true);
		if (message.authentication_confirmation == correct_token) {
			participant.authentication_status = AuthenticationStatus::Authenticated;
			
			if (m_interface) {
				m_interface->user_authenticated(sender, participant.long_term_public_key);
			}
		} else {
			participant.authentication_status = AuthenticationStatus::AuthenticationFailed;
			
			if (m_interface) {
				m_interface->user_authentication_failed(sender);
			}
		}
	} else if (np1sec_message.type == Message::Type::Authorization) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		AuthorizationMessage signed_message;
		try {
			signed_message = AuthorizationMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		if (!(
			   m_participants.count(message.username)
			&& m_participants.at(message.username).long_term_public_key == message.long_term_public_key
			&& m_participants.at(message.username).ephemeral_public_key == message.ephemeral_public_key
			&& m_participants.at(message.username).authorization_nonce == message.authorization_nonce
		)) {
			return;
		}
		
		Participant* authorized;
		Participant* unauthorized;
		if (m_participants[sender].authorized) {
			if (m_participants[message.username].authorized) {
				return;
			}
			authorized = &m_participants[sender];
			unauthorized = &m_participants[message.username];
		} else {
			if (!m_participants[message.username].authorized) {
				return;
			}
			authorized = &m_participants[message.username];
			unauthorized = &m_participants[sender];
		}
		assert(authorized->authorized);
		assert(!unauthorized->authorized);
		
		if (authorized->username == sender) {
			unauthorized->authorized_by.insert(authorized->username);
		} else {
			unauthorized->authorized_peers.insert(authorized->username);
		}
		
		if (m_interface) {
			m_interface->user_authorized_by(sender, message.username);
		}
		
		if (try_promote_unauthorized_participant(unauthorized)) {
			m_encrypted_chat.add_user(unauthorized->username, unauthorized->long_term_public_key);
		}
	} else if (np1sec_message.type == Message::Type::ConsistencyStatus) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		if (m_active && sender == m_room->username()) {
			UnsignedConsistencyCheckMessage message;
			message.channel_status_hash = m_channel_status_hash;
			send_message(ConsistencyCheckMessage::sign(message, m_ephemeral_private_key));
		}
		
		Event event;
		event.type = Message::Type::ConsistencyCheck;
		event.consistency_check.channel_status_hash = m_channel_status_hash;
		event.consistency_check.remaining_users.insert(sender);
		m_events.push_back(std::move(event));
	} else if (np1sec_message.type == Message::Type::ConsistencyCheck) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		ConsistencyCheckMessage signed_message;
		try {
			signed_message = ConsistencyCheckMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::ConsistencyCheck
			&& first_event->consistency_check.channel_status_hash == message.channel_status_hash
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->consistency_check.remaining_users.erase(sender);
		if (first_event->consistency_check.remaining_users.empty()) {
			m_events.erase(first_event);
		}
	} else if (np1sec_message.type == Message::Type::KeyExchangePublicKey) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		KeyExchangePublicKeyMessage signed_message;
		try {
			signed_message = KeyExchangePublicKeyMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::KeyExchangePublicKey
			&& first_event->key_event.key_id == message.key_id
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->key_event.remaining_users.erase(sender);
		if (first_event->key_event.remaining_users.empty()) {
			m_events.erase(first_event);
		}
		
		m_encrypted_chat.user_public_key(sender, message.key_id, message.public_key);
	} else if (np1sec_message.type == Message::Type::KeyExchangeSecretShare) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		KeyExchangeSecretShareMessage signed_message;
		try {
			signed_message = KeyExchangeSecretShareMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::KeyExchangeSecretShare
			&& first_event->key_event.key_id == message.key_id
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->key_event.remaining_users.erase(sender);
		if (first_event->key_event.remaining_users.empty()) {
			m_events.erase(first_event);
		}
		
		if (!m_encrypted_chat.have_key_exchange(message.key_id)) {
			return;
		}
		
		m_encrypted_chat.user_secret_share(sender, message.key_id, message.group_hash, message.secret_share);
	} else if (np1sec_message.type == Message::Type::KeyExchangeAcceptance) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		KeyExchangeAcceptanceMessage signed_message;
		try {
			signed_message = KeyExchangeAcceptanceMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::KeyExchangeAcceptance
			&& first_event->key_event.key_id == message.key_id
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->key_event.remaining_users.erase(sender);
		if (first_event->key_event.remaining_users.empty()) {
			m_events.erase(first_event);
		}
		
		if (!m_encrypted_chat.have_key_exchange(message.key_id)) {
			return;
		}
		
		m_encrypted_chat.user_key_hash(sender, message.key_id, message.key_hash);
	} else if (np1sec_message.type == Message::Type::KeyExchangeReveal) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		KeyExchangeRevealMessage signed_message;
		try {
			signed_message = KeyExchangeRevealMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::KeyExchangeReveal
			&& first_event->key_event.key_id == message.key_id
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->key_event.remaining_users.erase(sender);
		if (first_event->key_event.remaining_users.empty()) {
			m_events.erase(first_event);
		}
		
		if (!m_encrypted_chat.have_key_exchange(message.key_id)) {
			return;
		}
		
		m_encrypted_chat.user_private_key(sender, message.key_id, message.private_key);
	} else if (np1sec_message.type == Message::Type::KeyActivation) {
		if (!m_participants.count(sender)) {
			return;
		}
		
		KeyActivationMessage signed_message;
		try {
			signed_message = KeyActivationMessage::verify(np1sec_message, m_participants[sender].ephemeral_public_key);
		} catch(MessageFormatException) {
			return;
		}
		if (!signed_message.valid) {
			remove_user(sender);
			return;
		}
		auto message = signed_message.decode();
		
		auto first_event = first_user_event(sender);
		if (!(
			   first_event != m_events.end()
			&& first_event->type == Message::Type::KeyActivation
			&& first_event->key_event.key_id == message.key_id
		)) {
			remove_user(sender);
			return;
		}
		
		first_event->key_event.remaining_users.erase(sender);
		if (first_event->key_event.remaining_users.empty()) {
			m_events.erase(first_event);
		}
		
		if (m_encrypted_chat.have_session(message.key_id)) {
			m_encrypted_chat.user_activation(sender, message.key_id);
		}
	} else if (np1sec_message.type == Message::Type::Chat) {
		ChatMessage message;
		try {
			message = ChatMessage::decode(np1sec_message);
		} catch(MessageFormatException) {
			return;
		}
		
		m_encrypted_chat.decrypt_message(sender, message);
	}
}

void Channel::user_left(const std::string& username)
{
	hash_payload(username, 0, "left");
	
	remove_user(username);
}

void Channel::add_key_exchange_event(Message::Type type, const Hash& key_id, const std::set<std::string>& usernames)
{
	Event event;
	event.type = type;
	event.key_event.remaining_users = usernames;
	event.key_event.key_id = key_id;
	m_events.push_back(event);
}

void Channel::add_key_activation_event(const Hash& key_id, const std::set<std::string>& usernames)
{
	Event event;
	event.type = Message::Type::KeyActivation;
	event.key_event.key_id = key_id;
	event.key_event.remaining_users = usernames;
	m_events.push_back(event);
}



void Channel::self_joined()
{
	m_joined = true;
	
	for (const auto& i : m_participants) {
		if (i.second.username == m_room->username()) {
			continue;
		}
		
		authenticate_to(i.second.username, i.second.long_term_public_key, i.second.ephemeral_public_key, m_channel_status_hash);
	}
	
	if (m_interface) {
		m_interface->joined();
	}
}

bool Channel::try_promote_unauthorized_participant(Participant* participant)
{
	assert(!participant->authorized);
	
	for (const auto& i : m_participants) {
		if (i.second.authorized) {
			if (!participant->authorized_by.count(i.second.username)) {
				return false;
			}
			if (!participant->authorized_peers.count(i.second.username)) {
				return false;
			}
		}
	}
	participant->authorized = true;
	participant->authorized_by.clear();
	participant->authorized_peers.clear();
	
	if (participant->username == m_room->username()) {
		m_authorized = true;
	}
	
	if (m_interface) {
		m_interface->user_promoted(participant->username);
	}
	
	if (m_interface && participant->username == m_room->username()) {
		m_interface->authorized();
	}
	
	return true;
}

void Channel::remove_user(const std::string& username)
{
	std::set<std::string> usernames;
	usernames.insert(username);
	remove_users(usernames);
}

void Channel::remove_users(const std::set<std::string>& usernames)
{
	for (const std::string& username : usernames) {
		if (!m_participants.count(username)) {
			continue;
		}
		
		do_remove_user(username);
	}
	
	for (auto& p : m_participants) {
		if (!p.second.authorized) {
			if (try_promote_unauthorized_participant(&p.second)) {
				m_encrypted_chat.do_add_user(p.second.username, p.second.long_term_public_key);
				break;
			}
		}
	}
	
	m_encrypted_chat.remove_users(usernames);
}

void Channel::do_remove_user(const std::string& username)
{
	assert(m_participants.count(username));
	
	m_participants.erase(username);
	for (auto& p : m_participants) {
		if (!p.second.authorized) {
			p.second.authorized_by.erase(username);
			p.second.authorized_peers.erase(username);
		}
	}
	
	for (std::vector<Event>::iterator i = m_events.begin(); i != m_events.end(); ) {
		if (i->type == Message::Type::ChannelStatus) {
			i->channel_status.remaining_users.erase(username);
			if (i->channel_status.remaining_users.empty()) {
				i = m_events.erase(i);
			} else {
				++i;
			}
		} else if (i->type == Message::Type::ConsistencyCheck) {
			i->channel_status.remaining_users.erase(username);
			if (i->channel_status.remaining_users.empty()) {
				i = m_events.erase(i);
			} else {
				++i;
			}
		} else if (
			   i->type == Message::Type::KeyExchangePublicKey
			|| i->type == Message::Type::KeyExchangeSecretShare
			|| i->type == Message::Type::KeyExchangeAcceptance
			|| i->type == Message::Type::KeyExchangeReveal
			|| i->type == Message::Type::KeyActivation
		) {
			i->key_event.remaining_users.erase(username);
			if (i->key_event.remaining_users.empty()) {
				i = m_events.erase(i);
			} else {
				++i;
			}
		} else {
			++i;
		}
	}
	
	if (m_interface) {
		m_interface->user_left(username);
	}
}



void Channel::send_message(const Message& message)
{
	m_room->send_message(message);
}

Message Channel::channel_status(const std::string& searcher_username, const Hash& searcher_nonce) const
{
	ChannelStatusMessage result;
	result.searcher_username = searcher_username;
	result.searcher_nonce = searcher_nonce;
	result.channel_status_hash = m_channel_status_hash;
	
	for (const auto& i : m_participants) {
		if (i.second.authorized) {
			ChannelStatusMessage::Participant participant;
			participant.username = i.second.username;
			participant.long_term_public_key = i.second.long_term_public_key;
			participant.ephemeral_public_key = i.second.ephemeral_public_key;
			participant.authorization_nonce = i.second.authorization_nonce;
			result.participants.push_back(participant);
		} else {
			ChannelStatusMessage::UnauthorizedParticipant participant;
			participant.username = i.second.username;
			participant.long_term_public_key = i.second.long_term_public_key;
			participant.ephemeral_public_key = i.second.ephemeral_public_key;
			participant.authorization_nonce = i.second.authorization_nonce;
			participant.authorized_by = i.second.authorized_by;
			participant.authorized_peers = i.second.authorized_peers;
			result.unauthorized_participants.push_back(participant);
		}
	}
	
	result.key_exchanges = m_encrypted_chat.encode_key_exchanges();
	
	for (const Event& event : m_events) {
		if (event.type == Message::Type::ChannelStatus) {
			result.events.push_back(event.channel_status.encode(result));
		} else if (event.type == Message::Type::ConsistencyCheck) {
			result.events.push_back(event.consistency_check.encode(result));
		} else if (
			   event.type == Message::Type::KeyExchangePublicKey
			|| event.type == Message::Type::KeyExchangeSecretShare
			|| event.type == Message::Type::KeyExchangeAcceptance
			|| event.type == Message::Type::KeyExchangeReveal
		) {
			KeyExchangeEvent key_exchange_event;
			key_exchange_event.type = event.type;
			key_exchange_event.key_id = event.key_event.key_id;
			if (m_encrypted_chat.have_key_exchange(event.key_event.key_id)) {
				key_exchange_event.cancelled = false;
			} else {
				key_exchange_event.cancelled = true;
				key_exchange_event.remaining_users = event.key_event.remaining_users;
			}
			result.events.push_back(key_exchange_event.encode(result));
		} else if (event.type == Message::Type::KeyActivation) {
			KeyActivationEvent key_activation_event;
			key_activation_event.key_id = event.key_event.key_id;
			key_activation_event.remaining_users = event.key_event.remaining_users;
			result.events.push_back(key_activation_event.encode(result));
		} else {
			assert(false);
		}
	}
	
	return result.encode();
}

void Channel::hash_message(const std::string& sender, const Message& message)
{
	hash_payload(sender, uint8_t(message.type), message.payload);
}

void Channel::hash_payload(const std::string& sender, uint8_t type, const std::string& message)
{
	Hash zero;
	memset(zero.buffer, 0, sizeof(zero.buffer));
	
	std::string buffer = channel_status(std::string(), zero).payload;
	buffer += sender;
	buffer += type;
	buffer += message;
	m_channel_status_hash = crypto::hash(buffer);
}

void Channel::authenticate_to(const std::string& username, const PublicKey& long_term_public_key, const PublicKey& ephemeral_public_key, const Hash& nonce)
{
	AuthenticationMessage message;
	message.sender_long_term_public_key = m_room->long_term_public_key();
	message.sender_ephemeral_public_key = this->ephemeral_public_key();
	message.peer_username = username;
	message.peer_long_term_public_key = long_term_public_key;
	message.peer_ephemeral_public_key = ephemeral_public_key;
	message.nonce = nonce;
	message.authentication_confirmation = authentication_token(username, long_term_public_key, ephemeral_public_key, nonce, false);
	send_message(message.encode());
}

Hash Channel::authentication_token(const std::string& username, const PublicKey& long_term_public_key, const PublicKey& ephemeral_public_key, const Hash& nonce, bool for_peer)
{
	Hash token = crypto::triple_diffie_hellman(
		m_room->long_term_private_key(),
		m_ephemeral_private_key,
		long_term_public_key,
		ephemeral_public_key
	);
	std::string buffer = token.as_string();
	buffer += nonce.as_string();
	if (for_peer) {
		buffer += long_term_public_key.as_string();
		buffer += username;
	} else {
		buffer += m_room->long_term_public_key().as_string();
		buffer += m_room->username();
	}
	return crypto::hash(buffer);
}

std::vector<Channel::Event>::iterator Channel::first_user_event(const std::string& username)
{
	for (std::vector<Event>::iterator i = m_events.begin(); i != m_events.end(); ++i) {
		if (i->type == Message::Type::ChannelStatus) {
			if (i->channel_status.remaining_users.count(username)) {
				return i;
			}
		} else if (i->type == Message::Type::ConsistencyCheck) {
			if (i->consistency_check.remaining_users.count(username)) {
				return i;
			}
		} else if (
			   i->type == Message::Type::KeyExchangePublicKey
			|| i->type == Message::Type::KeyExchangeSecretShare
			|| i->type == Message::Type::KeyExchangeAcceptance
			|| i->type == Message::Type::KeyExchangeReveal
			|| i->type == Message::Type::KeyActivation
		) {
			if (i->key_event.remaining_users.count(username)) {
				return i;
			}
		} else {
			assert(false);
		}
	}
	
	return m_events.end();
}

/*
 * TODO: this is a placeholder for proper timer support later.
 */
void Channel::set_channel_status_timer()
{
	m_channel_status_timer = Timer(m_room->interface(), 10000, [this] {
		send_message(ConsistencyStatusMessage::encode());
		set_channel_status_timer();
	});
}

} // namespace np1sec
