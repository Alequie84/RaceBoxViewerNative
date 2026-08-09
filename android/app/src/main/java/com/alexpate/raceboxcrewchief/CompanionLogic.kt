package com.alexpate.raceboxcrewchief

import java.net.URI
import java.net.URLDecoder

const val COMPANION_CONTRACT = "racebox-companion-session-v1"
const val COMPANION_JOB_CONTROL_CONTRACT = "racebox-companion-job-control-v1"
const val OFFICIAL_RESULTS_PHOTO_CONTRACT = "racebox-official-results-photo-v1"
const val CONNECTION_CONTRACT = "racebox-crew-chief-connection-v1"
const val HEALTH_CONTRACT = "racebox-crew-chief-v14"
const val PROMPT_REVISION = "racebox-crew-chief-behavior-v14-public-1"
const val MAX_QUEUED_FOLLOW_UPS = 10

data class PairingLink(val gateway: String, val code: String)
data class GatewayUrlValidation(
    val valid: Boolean,
    val normalizedUrl: String = "",
    val error: String = "",
)
data class StoredConnectionMigration(
    val gatewayBaseUrl: String,
    val offlineDemo: Boolean,
    val requiresRepair: Boolean,
)

data class ChatMessage(
    val cursor: Long,
    val id: String,
    val role: String,
    val content: String,
    val origin: String,
    val createdAt: Long,
)

data class SessionCredentials(
    val sessionId: String,
    val token: String,
    val pairingId: String,
    val title: String,
    val gatewayBaseUrl: String = "",
    val offlineDemo: Boolean = false,
)

data class SessionSnapshot(
    val credentials: SessionCredentials?,
    val messages: List<ChatMessage>,
    val draft: String,
    val cursor: Long,
    val pendingJobId: String,
    val queuedFollowUps: List<String> = emptyList(),
    val lastActivityAt: Long = 0,
)

data class SessionCollection(
    val activeSessionId: String,
    val sessions: List<SessionSnapshot>,
)

data class SessionHistoryItem(
    val sessionId: String,
    val title: String,
    val messageCount: Int,
    val preview: String,
    val lastActivityAt: Long,
)

private fun isTailscaleIpv4(host: String): Boolean {
    val octets = host.split('.').mapNotNull { it.toIntOrNull() }
    return octets.size == 4 && octets.all { it in 0..255 } &&
        octets[0] == 100 && octets[1] in 64..127
}

fun validateGatewayUrl(value: String): GatewayUrlValidation {
    return try {
        val uri = URI(value.trim())
        val scheme = uri.scheme?.lowercase().orEmpty()
        val host = uri.host.orEmpty()
        if (scheme !in setOf("https", "http")) {
            return GatewayUrlValidation(false, error = "Use HTTPS, or HTTP on Tailscale")
        }
        if (host.isBlank() || uri.rawUserInfo != null || uri.rawQuery != null || uri.rawFragment != null) {
            return GatewayUrlValidation(false, error = "Gateway host is missing or contains embedded credentials")
        }
        if (uri.rawPath !in setOf("", "/")) {
            return GatewayUrlValidation(false, error = "Enter the gateway base URL without an API path")
        }
        if (scheme == "http" && !isTailscaleIpv4(host)) {
            return GatewayUrlValidation(
                false,
                error = "Cleartext HTTP is allowed only on Tailscale 100.64.0.0/10; use HTTPS otherwise",
            )
        }
        val normalized = URI(scheme, null, host, uri.port, null, null, null).toString()
        GatewayUrlValidation(true, normalized)
    } catch (_: Exception) {
        GatewayUrlValidation(false, error = "Enter a complete gateway URL")
    }
}

fun migrateStoredConnection(
    collectionVersion: Int,
    gatewayBaseUrl: String?,
    offlineDemo: Boolean,
): StoredConnectionMigration {
    if (offlineDemo) return StoredConnectionMigration("", true, false)
    val validated = gatewayBaseUrl?.let(::validateGatewayUrl)
    return if (collectionVersion >= 4 && validated?.valid == true) {
        StoredConnectionMigration(validated.normalizedUrl, false, false)
    } else {
        // v1-v3 never stored a portable gateway. Do not recreate the retired
        // developer address; keep the encrypted history and require a new QR.
        StoredConnectionMigration("", false, true)
    }
}

fun parsePairingLink(value: String): PairingLink? {
    return try {
        val uri = URI(value.trim())
        if (uri.scheme != "raceboxcc" || uri.host != "pair") return null
        val values = (uri.rawQuery ?: "")
            .split('&')
            .mapNotNull { item ->
                val parts = item.split('=', limit = 2)
                if (parts.size != 2) null else URLDecoder.decode(
                    parts[0], "UTF-8"
                ) to URLDecoder.decode(parts[1], "UTF-8")
            }
            .toMap()
        val gateway = values["gateway"]?.trimEnd('/') ?: return null
        val code = values["code"]?.filter(Char::isDigit) ?: return null
        val validated = validateGatewayUrl(gateway)
        if (!validated.valid || code.length != 6) return null
        PairingLink(validated.normalizedUrl, code)
    } catch (_: Exception) {
        null
    }
}

fun offlineDemoSnapshot(now: Long = System.currentTimeMillis()): SessionSnapshot {
    val credentials = SessionCredentials(
        sessionId = "offline-demo-richmond",
        token = "",
        pairingId = "offline-demo",
        title = "Richmond offline demo",
        gatewayBaseUrl = "",
        offlineDemo = true,
    )
    return SessionSnapshot(
        credentials = credentials,
        messages = listOf(
            ChatMessage(
                cursor = 1,
                id = "offline-demo-welcome",
                role = "assistant",
                content = "OFFLINE DEMO — Richmond fixture loaded. Ask what changed, whether the gain is reliable, or what to test next. Answers are scripted locally and make no network calls.",
                origin = "scripted-local-demo",
                createdAt = now,
            )
        ),
        draft = "",
        cursor = 1,
        pendingJobId = "",
        lastActivityAt = now,
    )
}

fun offlineDemoAnswer(question: String): String {
    val focus = when {
        question.contains("reliable", ignoreCase = true) ->
            "One comparison lap is not enough to call the gain reliable."
        question.contains("next", ignoreCase = true) || question.contains("test", ignoreCase = true) ->
            "Next test: repeat one change for at least three clean laps in each condition and compare median pace."
        else ->
            "The scripted Richmond comparison carries more speed through the flowing section, but association is not proof the setup caused it."
    }
    return "OFFLINE DEMO — $focus Check steering correction and corner-exit speed, and keep driver and track conditions as consistent as possible."
}

fun mergeMessages(
    current: List<ChatMessage>, incoming: List<ChatMessage>
): List<ChatMessage> {
    val byId = LinkedHashMap<String, ChatMessage>()
    current.forEach { if (it.id.isNotBlank()) byId[it.id] = it }
    incoming.forEach { if (it.id.isNotBlank()) byId[it.id] = it }
    return byId.values.sortedWith(compareBy<ChatMessage> { it.cursor }.thenBy { it.createdAt })
}

fun upsertSession(
    sessions: List<SessionSnapshot>, incoming: SessionSnapshot
): List<SessionSnapshot> {
    val sessionId = incoming.credentials?.sessionId ?: return sessions
    return sessions.filterNot { it.credentials?.sessionId == sessionId } + incoming
}

fun removeSession(
    collection: SessionCollection, sessionId: String
): SessionCollection {
    val remaining = collection.sessions.filterNot { it.credentials?.sessionId == sessionId }
    val nextActive = if (collection.activeSessionId == sessionId) {
        sessionHistoryItems(remaining).firstOrNull()?.sessionId.orEmpty()
    } else {
        collection.activeSessionId
    }
    return SessionCollection(nextActive, remaining)
}

fun activeSession(collection: SessionCollection): SessionSnapshot? {
    return collection.sessions.firstOrNull {
        it.credentials?.sessionId == collection.activeSessionId
    } ?: sessionHistoryItems(collection.sessions).firstOrNull()?.let { item ->
        collection.sessions.firstOrNull { it.credentials?.sessionId == item.sessionId }
    }
}

fun sessionHistoryItems(sessions: List<SessionSnapshot>): List<SessionHistoryItem> {
    return sessions.mapNotNull { snapshot ->
        val credentials = snapshot.credentials ?: return@mapNotNull null
        val latestMessage = snapshot.messages.lastOrNull()
        SessionHistoryItem(
            sessionId = credentials.sessionId,
            title = credentials.title.ifBlank { "RaceBox session" },
            messageCount = snapshot.messages.size,
            preview = latestMessage?.content
                ?.replace(Regex("\\s+"), " ")
                ?.trim()
                ?.take(90)
                .orEmpty(),
            lastActivityAt = snapshot.lastActivityAt,
        )
    }.sortedWith(
        compareByDescending<SessionHistoryItem> { it.lastActivityAt }
            .thenBy { it.title.lowercase() }
            .thenBy { it.sessionId }
    )
}

fun inferredActivityTime(messages: List<ChatMessage>): Long {
    val supplied = messages.maxOfOrNull { it.createdAt } ?: return 0
    return if (supplied in 1 until 1_000_000_000_000L) supplied * 1_000 else supplied
}

fun enqueueFollowUp(current: List<String>, text: String): List<String> {
    val bounded = text.trim().take(4_000)
    if (bounded.isBlank() || current.size >= MAX_QUEUED_FOLLOW_UPS) return current
    return current + bounded
}
