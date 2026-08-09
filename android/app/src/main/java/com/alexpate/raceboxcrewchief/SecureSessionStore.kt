package com.alexpate.raceboxcrewchief

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Encrypts every paired session token, draft, cursor, and small resumable chat
 * cache with a non-exportable Android Keystore key. Session IDs and titles are
 * encrypted too. No analytics or service credential is accepted by this store.
 */
class SecureSessionStore(context: Context) {
    private val preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    /** Loads all saved conversations and migrates the original single-session cache in place. */
    fun load(): SessionCollection {
        val encodedIndex = preferences.getString(SESSION_INDEX, null)
            ?: return migrateLegacySnapshot()
        return try {
            val index = decodeIndex(JSONObject(decrypt(encodedIndex)))
            val sessions = index.sessionIds.mapNotNull { sessionId ->
                val encoded = preferences.getString(sessionKey(sessionId), null) ?: return@mapNotNull null
                try {
                    decodeSnapshot(JSONObject(decrypt(encoded)))
                } catch (_: Exception) {
                    null
                }
            }
            val usableIds = sessions.mapNotNull { it.credentials?.sessionId }.toSet()
            val activeId = if (index.activeSessionId in usableIds) {
                index.activeSessionId
            } else {
                sessionHistoryItems(sessions).firstOrNull()?.sessionId.orEmpty()
            }
            SessionCollection(activeId, sessions)
        } catch (_: Exception) {
            // Do not delete encrypted session blobs if the index is damaged.
            SessionCollection("", emptyList())
        }
    }

    /** Upserts one conversation without touching any other saved session. */
    fun save(snapshot: SessionSnapshot, makeActive: Boolean = true) {
        val credentials = snapshot.credentials ?: return
        val existingIndex = readIndex()
        val sessionIds = (existingIndex.sessionIds + credentials.sessionId).distinct()
        val activeId = if (makeActive) credentials.sessionId else existingIndex.activeSessionId
        preferences.edit()
            .putString(sessionKey(credentials.sessionId), encrypt(encodeSnapshot(snapshot).toString()))
            .putString(SESSION_INDEX, encrypt(encodeIndex(activeId, sessionIds).toString()))
            .remove(LEGACY_SNAPSHOT)
            .apply()
    }

    fun setActive(sessionId: String) {
        val index = readIndex()
        if (sessionId !in index.sessionIds) return
        preferences.edit()
            .putString(SESSION_INDEX, encrypt(encodeIndex(sessionId, index.sessionIds).toString()))
            .apply()
    }

    /** Forgets only this phone pairing. It never clears the server transcript. */
    fun remove(sessionId: String) {
        val index = readIndex()
        val remaining = index.sessionIds.filterNot { it == sessionId }
        val activeId = if (index.activeSessionId == sessionId) remaining.firstOrNull().orEmpty()
        else index.activeSessionId
        preferences.edit()
            .remove(sessionKey(sessionId))
            .putString(SESSION_INDEX, encrypt(encodeIndex(activeId, remaining).toString()))
            .apply()
    }

    private fun migrateLegacySnapshot(): SessionCollection {
        val encoded = preferences.getString(LEGACY_SNAPSHOT, null)
            ?: return SessionCollection("", emptyList())
        return try {
            val snapshot = decodeSnapshot(JSONObject(decrypt(encoded)))
            val credentials = snapshot.credentials
            if (credentials == null) {
                preferences.edit().remove(LEGACY_SNAPSHOT).apply()
                SessionCollection("", emptyList())
            } else {
                val migrated = snapshot.copy(
                    lastActivityAt = snapshot.lastActivityAt.takeIf { it > 0 }
                        ?: inferredActivityTime(snapshot.messages),
                )
                preferences.edit()
                    .putString(sessionKey(credentials.sessionId), encrypt(encodeSnapshot(migrated).toString()))
                    .putString(
                        SESSION_INDEX,
                        encrypt(encodeIndex(credentials.sessionId, listOf(credentials.sessionId)).toString()),
                    )
                    .remove(LEGACY_SNAPSHOT)
                    .apply()
                SessionCollection(credentials.sessionId, listOf(migrated))
            }
        } catch (_: Exception) {
            // Match the old behavior for an unreadable legacy cache only.
            preferences.edit().remove(LEGACY_SNAPSHOT).apply()
            SessionCollection("", emptyList())
        }
    }

    private data class StoredIndex(val activeSessionId: String, val sessionIds: List<String>)

    private fun readIndex(): StoredIndex {
        val encoded = preferences.getString(SESSION_INDEX, null) ?: return StoredIndex("", emptyList())
        return try {
            decodeIndex(JSONObject(decrypt(encoded)))
        } catch (_: Exception) {
            StoredIndex("", emptyList())
        }
    }

    private fun encodeIndex(activeSessionId: String, sessionIds: List<String>): JSONObject {
        return JSONObject()
            .put("version", COLLECTION_VERSION)
            .put("active_session_id", activeSessionId)
            .put("session_ids", JSONArray().apply { sessionIds.forEach(::put) })
    }

    private fun decodeIndex(root: JSONObject): StoredIndex {
        if (root.optInt("version") !in SUPPORTED_COLLECTION_VERSIONS) {
            throw IllegalArgumentException("Unsupported session index")
        }
        val ids = mutableListOf<String>()
        val supplied = root.optJSONArray("session_ids") ?: JSONArray()
        for (index in 0 until supplied.length()) {
            supplied.optString(index).takeIf { it.isNotBlank() }?.let(ids::add)
        }
        return StoredIndex(root.optString("active_session_id"), ids.distinct())
    }

    private fun encodeSnapshot(snapshot: SessionSnapshot): JSONObject {
        val root = JSONObject()
            .put("version", COLLECTION_VERSION)
            .put("draft", snapshot.draft.take(MAX_TEXT))
            .put("cursor", snapshot.cursor)
            .put("pending_job_id", snapshot.pendingJobId.take(120))
            .put(
                "queued_follow_ups",
                JSONArray().apply {
                    snapshot.queuedFollowUps.take(MAX_QUEUED_FOLLOW_UPS).forEach {
                        put(it.take(MAX_TEXT))
                    }
                },
            )
            .put("last_activity_at", snapshot.lastActivityAt)
        snapshot.credentials?.let { credentials ->
            root.put(
                "credentials",
                JSONObject()
                    .put("session_id", credentials.sessionId)
                    .put("token", credentials.token)
                    .put("pairing_id", credentials.pairingId)
                    .put("title", credentials.title.take(200))
                    .put("gateway_base_url", credentials.gatewayBaseUrl.take(500))
                    .put("offline_demo", credentials.offlineDemo),
            )
        }
        root.put(
            "messages",
            JSONArray().apply {
                snapshot.messages.takeLast(MAX_MESSAGES).forEach { message ->
                    put(
                        JSONObject()
                            .put("cursor", message.cursor)
                            .put("id", message.id.take(120))
                            .put("role", message.role)
                            .put("content", message.content.take(MAX_TEXT))
                            .put("origin", message.origin.take(80))
                            .put("created_at", message.createdAt),
                    )
                }
            },
        )
        return root
    }

    private fun decodeSnapshot(root: JSONObject): SessionSnapshot {
        val version = root.optInt("version")
        if (version != LEGACY_VERSION && version !in SUPPORTED_COLLECTION_VERSIONS) {
            throw IllegalArgumentException("Unsupported cache")
        }
        val credentials = root.optJSONObject("credentials")?.let {
            val connection = migrateStoredConnection(
                version,
                it.optString("gateway_base_url").takeIf(String::isNotBlank),
                it.optBoolean("offline_demo"),
            )
            SessionCredentials(
                sessionId = it.getString("session_id"),
                token = it.getString("token"),
                pairingId = it.getString("pairing_id"),
                title = it.optString("title", "RaceBox session"),
                gatewayBaseUrl = connection.gatewayBaseUrl,
                offlineDemo = connection.offlineDemo,
            )
        }
        val messages = mutableListOf<ChatMessage>()
        val supplied = root.optJSONArray("messages") ?: JSONArray()
        for (index in 0 until supplied.length()) {
            val item = supplied.optJSONObject(index) ?: continue
            val id = item.optString("id")
            val content = item.optString("content")
            if (id.isBlank() || content.isBlank()) continue
            messages += ChatMessage(
                cursor = item.optLong("cursor"),
                id = id,
                role = if (item.optString("role") == "assistant") "assistant" else "user",
                content = content.take(MAX_TEXT),
                origin = item.optString("origin").take(80),
                createdAt = item.optLong("created_at"),
            )
        }
        val merged = mergeMessages(emptyList(), messages)
        val queuedFollowUps = mutableListOf<String>()
        val suppliedFollowUps = root.optJSONArray("queued_follow_ups") ?: JSONArray()
        for (index in 0 until minOf(suppliedFollowUps.length(), MAX_QUEUED_FOLLOW_UPS)) {
            suppliedFollowUps.optString(index).trim().take(MAX_TEXT)
                .takeIf { it.isNotBlank() }
                ?.let(queuedFollowUps::add)
        }
        return SessionSnapshot(
            credentials = credentials,
            messages = merged,
            draft = root.optString("draft").take(MAX_TEXT),
            cursor = root.optLong("cursor"),
            pendingJobId = root.optString("pending_job_id").take(120),
            queuedFollowUps = queuedFollowUps,
            lastActivityAt = root.optLong("last_activity_at").takeIf { it > 0 }
                ?: inferredActivityTime(merged),
        )
    }

    private fun sessionKey(sessionId: String): String {
        val hash = MessageDigest.getInstance("SHA-256")
            .digest(sessionId.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        return SESSION_PREFIX + hash
    }

    private fun encrypt(value: String): String {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val iv = Base64.encodeToString(cipher.iv, Base64.NO_WRAP)
        val content = Base64.encodeToString(cipher.doFinal(value.toByteArray()), Base64.NO_WRAP)
        return "$iv.$content"
    }

    private fun decrypt(value: String): String {
        val parts = value.split('.', limit = 2)
        require(parts.size == 2)
        val iv = Base64.decode(parts[0], Base64.NO_WRAP)
        val content = Base64.decode(parts[1], Base64.NO_WRAP)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
        return String(cipher.doFinal(content), Charsets.UTF_8)
    }

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(
                KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setRandomizedEncryptionRequired(true)
                    .build(),
            )
            generateKey()
        }
    }

    companion object {
        private const val PREFERENCES = "racebox_crew_chief_secure_state"
        private const val LEGACY_SNAPSHOT = "encrypted_snapshot_v1"
        private const val SESSION_INDEX = "encrypted_session_index_v2"
        private const val SESSION_PREFIX = "encrypted_session_v2_"
        private const val KEY_ALIAS = "racebox_crew_chief_device_key_v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val LEGACY_VERSION = 1
        private const val PREVIOUS_COLLECTION_VERSION = 3
        private const val COLLECTION_VERSION = 4
        private val SUPPORTED_COLLECTION_VERSIONS = setOf(2, PREVIOUS_COLLECTION_VERSION, COLLECTION_VERSION)
        private const val MAX_MESSAGES = 2_000
        private const val MAX_TEXT = 4_000
    }
}
