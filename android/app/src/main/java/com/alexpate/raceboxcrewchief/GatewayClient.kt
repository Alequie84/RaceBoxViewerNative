package com.alexpate.raceboxcrewchief

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

data class PairResult(
    val credentials: SessionCredentials,
)

data class MessagePage(
    val messages: List<ChatMessage>,
    val nextCursor: Long,
    val total: Int,
)

data class SubmitResult(val jobId: String, val status: String)
data class JobResult(val status: String, val error: String)
data class CancelResult(val cancelled: Boolean, val status: String)
data class ClearResult(val backupName: String, val clearedMessages: Int)

class GatewayClient(baseUrl: String) {
    private val baseUrl: String

    init {
        val validated = validateGatewayUrl(baseUrl)
        require(validated.valid) { validated.error }
        this.baseUrl = validated.normalizedUrl
    }

    fun pair(code: String, deviceName: String): PairResult {
        val body = JSONObject()
            .put("contract", COMPANION_CONTRACT)
            .put("code", code.filter(Char::isDigit))
            .put("device_name", deviceName.take(120))
        val response = request("POST", "/v1/crew-chief/companion/pair", body)
        requireContract(response)
        return PairResult(
            SessionCredentials(
                response.getString("session_id"),
                response.getString("session_token"),
                response.getString("pairing_id"),
                response.optString("title", "RaceBox session"),
                baseUrl,
            )
        )
    }

    fun messages(credentials: SessionCredentials, after: Long): MessagePage {
        val response = request(
            "GET",
            "/v1/crew-chief/companion/sessions/${credentials.sessionId}/messages?after=$after",
            token = credentials.token,
        )
        requireContract(response)
        val messages = mutableListOf<ChatMessage>()
        val supplied = response.optJSONArray("messages") ?: JSONArray()
        for (index in 0 until supplied.length()) {
            val item = supplied.optJSONObject(index) ?: continue
            val id = item.optString("id")
            val content = item.optString("content")
            if (id.isBlank() || content.isBlank()) continue
            messages += ChatMessage(
                cursor = item.optLong("cursor"),
                id = id,
                role = if (item.optString("role") == "assistant") "assistant" else "user",
                content = content.take(4_000),
                origin = item.optString("origin").take(80),
                createdAt = item.optLong("created_at"),
            )
        }
        return MessagePage(
            messages,
            response.optLong("next_cursor", after),
            response.optInt("total", messages.size),
        )
    }

    fun submit(
        credentials: SessionCredentials,
        messageId: String,
        text: String,
        officialResultsPhoto: OfficialResultsPhoto? = null,
    ): SubmitResult {
        val body = JSONObject()
            .put("contract", COMPANION_CONTRACT)
            .put("message_id", messageId.take(120))
            .put("text", text.take(4_000))
        officialResultsPhoto?.let { photo ->
            if (photo.bytes.isEmpty() || photo.bytes.size > MAX_OFFICIAL_RESULTS_PHOTO_BYTES) {
                throw IOException("Official results photo is too large")
            }
            body.put(
                "attachment",
                JSONObject()
                    .put("contract", OFFICIAL_RESULTS_PHOTO_CONTRACT)
                    .put("mime_type", photo.mimeType)
                    .put("sha256", photo.sha256)
                    .put("data_base64", Base64.encodeToString(photo.bytes, Base64.NO_WRAP)),
            )
        }
        val response = request(
            "POST",
            "/v1/crew-chief/companion/sessions/${credentials.sessionId}/messages",
            body,
            credentials.token,
        )
        requireContract(response)
        return SubmitResult(response.getString("job_id"), response.optString("status", "queued"))
    }

    fun job(credentials: SessionCredentials, jobId: String): JobResult {
        val response = request(
            "GET", "/v1/crew-chief/companion/jobs/$jobId", token = credentials.token
        )
        requireContract(response)
        return JobResult(response.optString("status", "failed"), response.optString("error"))
    }

    fun cancel(credentials: SessionCredentials, jobId: String): CancelResult {
        val response = request(
            "POST",
            "/v1/crew-chief/companion/jobs/$jobId/cancel",
            JSONObject().put("contract", COMPANION_JOB_CONTROL_CONTRACT),
            credentials.token,
        )
        requireContract(response)
        if (response.optString("control_contract") != COMPANION_JOB_CONTROL_CONTRACT) {
            throw IOException("Gateway job-control contract did not match")
        }
        return CancelResult(
            response.optBoolean("cancelled"),
            response.optString("status", "failed"),
        )
    }

    fun clear(credentials: SessionCredentials): ClearResult {
        val response = request(
            "DELETE",
            "/v1/crew-chief/companion/sessions/${credentials.sessionId}/messages",
            token = credentials.token,
        )
        requireContract(response)
        if (!response.optBoolean("backup_created")) throw IOException("Server did not confirm the transcript backup")
        return ClearResult(
            response.optString("backup_name"), response.optInt("cleared_messages")
        )
    }

    fun unpair(credentials: SessionCredentials) {
        val response = request(
            "DELETE",
            "/v1/crew-chief/companion/pairing/${credentials.pairingId}?session_id=${credentials.sessionId}",
            token = credentials.token,
        )
        requireContract(response)
        if (!response.optBoolean("revoked")) throw IOException("Phone pairing was not revoked")
    }

    private fun requireContract(response: JSONObject) {
        if (response.optString("contract") != COMPANION_CONTRACT) {
            throw IOException("Gateway companion contract did not match")
        }
    }

    private fun request(
        method: String,
        path: String,
        body: JSONObject? = null,
        token: String = "",
    ): JSONObject {
        require(path.startsWith('/'))
        val url = URL(baseUrl + path)
        val validated = validateGatewayUrl(baseUrl)
        if (!validated.valid || validated.normalizedUrl != baseUrl) {
            throw IOException("Refusing an unsafe gateway address")
        }
        val connection = (url.openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 10_000
            readTimeout = 20_000
            setRequestProperty("Accept", "application/json")
            if (token.isNotBlank()) setRequestProperty("Authorization", "Bearer $token")
            if (body != null) {
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
            }
        }
        try {
            if (body != null) {
                connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            val response = if (text.isBlank()) JSONObject() else JSONObject(text)
            if (status !in 200..299) {
                throw IOException(response.optString("error", "Gateway returned HTTP $status"))
            }
            return response
        } finally {
            connection.disconnect()
        }
    }
}
