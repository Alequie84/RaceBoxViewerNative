package com.alexpate.raceboxcrewchief

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.setContent
import androidx.core.content.FileProvider
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.google.android.gms.tasks.Task
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import java.io.IOException
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

private const val PENDING_CAMERA_PATH = "pending_official_results_camera_path"

private data class RetainedPhotoState(
    val photo: OfficialResultsPhoto?,
)

private data class AppUiState(
    val credentials: SessionCredentials? = null,
    val messages: List<ChatMessage> = emptyList(),
    val draft: String = "",
    val cursor: Long = 0,
    val pendingJobId: String = "",
    val queuedFollowUps: List<String> = emptyList(),
    val lastActivityAt: Long = 0,
    val sessionHistory: List<SessionHistoryItem> = emptyList(),
    val manualCode: String = "",
    val gatewayUrl: String = "",
    val status: String = "Pair with RaceBox Viewer",
    val sendInFlight: Boolean = false,
    val stopInFlight: Boolean = false,
    val pairingMode: Boolean = false,
    val showHistory: Boolean = false,
    val showInfo: Boolean = false,
    val confirmClear: Boolean = false,
    val showPhotoSource: Boolean = false,
    val photoPreparing: Boolean = false,
    val officialResultsPhoto: OfficialResultsPhoto? = null,
    val confirmGateway: Boolean = false,
)

class MainActivity : ComponentActivity() {
    private val worker = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())
    private val networkBusy = AtomicBoolean(false)
    private lateinit var secureStore: SecureSessionStore
    private var sessionCollection = SessionCollection("", emptyList())
    private var foreground = false
    private var state by mutableStateOf(AppUiState())
    private var pendingCameraFile: File? = null

    private val takeResultsPhoto = registerForActivityResult(
        ActivityResultContracts.TakePicture()
    ) { captured ->
        val file = pendingCameraFile
        pendingCameraFile = null
        if (captured && file != null) {
            prepareResultsPhoto(
                FileProvider.getUriForFile(
                    this,
                    "${BuildConfig.APPLICATION_ID}.fileprovider",
                    file,
                ),
                file,
            )
        } else {
            file?.delete()
            state = state.copy(status = "Photo cancelled")
        }
    }

    private val chooseResultsPhoto = registerForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri: Uri? ->
        if (uri == null) {
            state = state.copy(status = "Photo cancelled")
        } else {
            prepareResultsPhoto(uri)
        }
    }

    private val pollRunnable = Runnable { pollOnce() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pendingCameraFile = savedInstanceState
            ?.getString(PENDING_CAMERA_PATH)
            ?.let(::File)
        secureStore = SecureSessionStore(this)
        sessionCollection = secureStore.load()
        val cached = activeSession(sessionCollection)
        state = stateFromSnapshot(
            cached,
            if (cached == null) "Pair with RaceBox Viewer" else "Connecting...",
        )
        @Suppress("DEPRECATION")
        val retainedPhoto = lastCustomNonConfigurationInstance as? RetainedPhotoState
        retainedPhoto?.photo?.let { photo ->
            state = state.copy(
                officialResultsPhoto = photo,
                status = "Results photo ready",
            )
        }
        acceptPairingIntent(intent)
        setContent {
            RaceBoxTheme {
                CompanionApp(
                    state = state,
                    onDraftChanged = { draft ->
                        state = state.copy(draft = draft.take(4_000))
                        persist()
                    },
                    onManualCodeChanged = { code ->
                        state = state.copy(manualCode = code.filter(Char::isDigit).take(6))
                    },
                    onGatewayChanged = { gateway ->
                        state = state.copy(gatewayUrl = gateway.take(500))
                    },
                    onPair = { pair(state.manualCode) },
                    onScan = ::scanPairingCode,
                    onOfflineDemo = ::startOfflineDemo,
                    onConfirmGateway = {
                        val code = state.manualCode
                        val gateway = state.gatewayUrl
                        state = state.copy(confirmGateway = false)
                        pair(code, gateway, confirmed = true)
                    },
                    onCancelGateway = { state = state.copy(confirmGateway = false) },
                    onSend = ::sendMessage,
                    onStop = ::stopResponse,
                    onShowPhotoSource = {
                        state = state.copy(showPhotoSource = true)
                    },
                    onHidePhotoSource = {
                        state = state.copy(showPhotoSource = false)
                    },
                    onTakeResultsPhoto = ::launchResultsCamera,
                    onChooseResultsPhoto = ::launchResultsPicker,
                    onRemoveResultsPhoto = {
                        state = state.copy(
                            officialResultsPhoto = null,
                            status = "Connected",
                        )
                    },
                    onShowHistory = { state = state.copy(showHistory = true) },
                    onHideHistory = { state = state.copy(showHistory = false) },
                    onSelectSession = ::switchSession,
                    onPairAnother = ::showPairing,
                    onCancelPairing = ::cancelPairing,
                    onShowInfo = { state = state.copy(showInfo = true) },
                    onHideInfo = { state = state.copy(showInfo = false) },
                    onRequestClear = { state = state.copy(confirmClear = true) },
                    onCancelClear = { state = state.copy(confirmClear = false) },
                    onConfirmClear = ::clearConversation,
                    onUnpair = ::unpair,
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        acceptPairingIntent(intent)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        pendingCameraFile?.let { outState.putString(PENDING_CAMERA_PATH, it.absolutePath) }
        super.onSaveInstanceState(outState)
    }

    @Suppress("OVERRIDE_DEPRECATION")
    override fun onRetainCustomNonConfigurationInstance(): Any =
        RetainedPhotoState(state.officialResultsPhoto)

    override fun onStart() {
        super.onStart()
        foreground = true
        schedulePoll(0)
    }

    override fun onStop() {
        foreground = false
        handler.removeCallbacks(pollRunnable)
        persist()
        super.onStop()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        worker.shutdownNow()
        super.onDestroy()
    }

    private fun stateFromSnapshot(snapshot: SessionSnapshot?, status: String): AppUiState {
        val suppliedCredentials = snapshot?.credentials
        val usableCredentials = suppliedCredentials?.takeIf {
            it.offlineDemo || validateGatewayUrl(it.gatewayBaseUrl).valid
        }
        return AppUiState(
            credentials = usableCredentials,
            messages = snapshot?.messages.orEmpty(),
            draft = snapshot?.draft.orEmpty(),
            cursor = snapshot?.cursor ?: 0,
            pendingJobId = snapshot?.pendingJobId.orEmpty(),
            queuedFollowUps = snapshot?.queuedFollowUps.orEmpty(),
            lastActivityAt = snapshot?.lastActivityAt ?: 0,
            sessionHistory = sessionHistoryItems(sessionCollection.sessions),
            gatewayUrl = suppliedCredentials?.gatewayBaseUrl.orEmpty(),
            status = if (suppliedCredentials != null && usableCredentials == null) {
                "This older pairing has no public gateway address. Scan a new Viewer QR code."
            } else status,
            pairingMode = suppliedCredentials != null && usableCredentials == null,
        )
    }

    private fun currentSnapshot(): SessionSnapshot? {
        val credentials = state.credentials ?: return null
        return SessionSnapshot(
            credentials = credentials,
            messages = state.messages,
            draft = state.draft,
            cursor = state.cursor,
            pendingJobId = state.pendingJobId,
            queuedFollowUps = state.queuedFollowUps,
            lastActivityAt = state.lastActivityAt,
        )
    }

    private fun storeSnapshot(snapshot: SessionSnapshot, makeActive: Boolean) {
        sessionCollection = SessionCollection(
            activeSessionId = if (makeActive) {
                snapshot.credentials?.sessionId.orEmpty()
            } else {
                sessionCollection.activeSessionId
            },
            sessions = upsertSession(sessionCollection.sessions, snapshot),
        )
        secureStore.save(snapshot, makeActive)
    }

    private fun showPairing() {
        persist()
        handler.removeCallbacks(pollRunnable)
        state = state.copy(
            pairingMode = true,
            showHistory = false,
            showInfo = false,
            manualCode = "",
            gatewayUrl = "",
            status = "Pair another Viewer session",
        )
    }

    private fun cancelPairing() {
        if (state.credentials == null) return
        state = state.copy(
            pairingMode = false,
            manualCode = "",
            status = if (state.credentials?.offlineDemo == true) "Offline demo" else "Connected",
        )
        schedulePoll(0)
    }

    private fun switchSession(sessionId: String) {
        persist()
        val snapshot = sessionCollection.sessions.firstOrNull {
            it.credentials?.sessionId == sessionId
        } ?: return
        sessionCollection = sessionCollection.copy(activeSessionId = sessionId)
        secureStore.setActive(sessionId)
        handler.removeCallbacks(pollRunnable)
        state = stateFromSnapshot(
            snapshot,
            if (snapshot.credentials?.offlineDemo == true) "Offline demo" else "Connecting...",
        )
        schedulePoll(0)
    }

    private fun acceptPairingIntent(intent: Intent?) {
        val value = intent?.dataString ?: return
        val pairing = parsePairingLink(value)
        if (pairing == null) {
            state = state.copy(status = "That pairing link is malformed or uses an unsafe gateway")
            return
        }
        handler.removeCallbacks(pollRunnable)
        state = state.copy(
            manualCode = pairing.code,
            gatewayUrl = pairing.gateway,
            pairingMode = state.credentials != null,
        )
        pair(pairing.code, pairing.gateway)
    }

    private fun scanPairingCode() {
        val options = GmsBarcodeScannerOptions.Builder()
            .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
            .enableAutoZoom()
            .build()
        val task: Task<Barcode> = GmsBarcodeScanning.getClient(this, options).startScan()
        task.addOnSuccessListener { barcode ->
            val pairing = barcode.rawValue?.let(::parsePairingLink)
            if (pairing == null) {
                state = state.copy(status = "That QR code is not a RaceBox pairing code")
            } else {
                state = state.copy(manualCode = pairing.code, gatewayUrl = pairing.gateway)
                pair(pairing.code, pairing.gateway)
            }
        }.addOnFailureListener { error ->
            state = state.copy(status = classifyError(error))
        }
    }

    private fun launchResultsCamera() {
        val directory = File(cacheDir, "official-results-photos")
        if (!directory.exists() && !directory.mkdirs()) {
            state = state.copy(
                showPhotoSource = false,
                status = "Could not open photo storage",
            )
            return
        }
        val file = File.createTempFile("official-results-", ".jpg", directory)
        pendingCameraFile = file
        val uri = FileProvider.getUriForFile(
            this,
            "${BuildConfig.APPLICATION_ID}.fileprovider",
            file,
        )
        state = state.copy(showPhotoSource = false, status = "Opening camera...")
        takeResultsPhoto.launch(uri)
    }

    private fun launchResultsPicker() {
        state = state.copy(showPhotoSource = false, status = "Choose a results photo")
        chooseResultsPhoto.launch("image/*")
    }

    private fun prepareResultsPhoto(uri: Uri, temporaryFile: File? = null) {
        state = state.copy(photoPreparing = true, status = "Preparing results photo...")
        worker.execute {
            try {
                val prepared = prepareOfficialResultsPhoto(this, uri)
                runOnUiThread {
                    state = state.copy(
                        officialResultsPhoto = prepared,
                        photoPreparing = false,
                        status = "Results photo ready",
                    )
                }
            } catch (error: Exception) {
                runOnUiThread {
                    state = state.copy(
                        photoPreparing = false,
                        status = error.message?.take(160) ?: "Could not prepare photo",
                    )
                }
            } finally {
                temporaryFile?.delete()
            }
        }
    }

    private fun pair(
        code: String,
        requestedGateway: String = state.gatewayUrl,
        confirmed: Boolean = false,
    ) {
        if (code.length != 6) {
            state = state.copy(status = "Enter the six-digit code from Viewer")
            return
        }
        val validated = validateGatewayUrl(requestedGateway)
        if (!validated.valid) {
            state = state.copy(status = validated.error)
            return
        }
        val familiar = sessionCollection.sessions.any {
            it.credentials?.gatewayBaseUrl == validated.normalizedUrl
        }
        if (!familiar && !confirmed) {
            state = state.copy(
                gatewayUrl = validated.normalizedUrl,
                manualCode = code,
                confirmGateway = true,
                status = "Confirm this gateway before connecting",
            )
            return
        }
        if (!networkBusy.compareAndSet(false, true)) {
            state = state.copy(status = "Finishing the current sync - tap Pair again")
            return
        }
        state = state.copy(status = "Pairing…")
        worker.execute {
            try {
                val result = GatewayClient(validated.normalizedUrl)
                    .pair(code, "Android ${Build.MODEL}".take(120))
                runOnUiThread {
                    val snapshot = SessionSnapshot(
                        credentials = result.credentials,
                        messages = emptyList(),
                        draft = "",
                        cursor = 0,
                        pendingJobId = "",
                        lastActivityAt = System.currentTimeMillis(),
                    )
                    storeSnapshot(snapshot, makeActive = true)
                    state = stateFromSnapshot(snapshot, "Connected")
                    schedulePoll(0)
                }
            } catch (error: Exception) {
                runOnUiThread { state = state.copy(status = classifyError(error)) }
            } finally {
                networkBusy.set(false)
            }
        }
    }

    private fun startOfflineDemo() {
        handler.removeCallbacks(pollRunnable)
        val snapshot = offlineDemoSnapshot()
        storeSnapshot(snapshot, makeActive = true)
        state = stateFromSnapshot(snapshot, "Offline demo — no network calls")
    }

    private fun sendMessage() {
        val credentials = state.credentials ?: return
        val draft = state.draft.trim()
        if (credentials.offlineDemo) {
            if (draft.isBlank()) return
            val now = System.currentTimeMillis()
            val nextCursor = state.cursor + 1
            val user = ChatMessage(
                nextCursor, "offline-user-${UUID.randomUUID()}", "user", draft,
                "phone-offline-demo", now,
            )
            val assistant = ChatMessage(
                nextCursor + 1, "offline-answer-${UUID.randomUUID()}", "assistant",
                offlineDemoAnswer(draft), "scripted-local-demo", now + 1,
            )
            state = state.copy(
                messages = state.messages + user + assistant,
                draft = "",
                cursor = nextCursor + 1,
                officialResultsPhoto = null,
                lastActivityAt = now,
                status = "Offline demo — scripted Richmond answer",
            )
            persist()
            return
        }
        if (state.pendingJobId.isNotBlank()) {
            if (draft.isBlank() || state.sendInFlight || state.stopInFlight) return
            val queued = enqueueFollowUp(state.queuedFollowUps, draft)
            if (queued == state.queuedFollowUps) {
                state = state.copy(status = "Follow-up queue is full")
                return
            }
            state = state.copy(
                draft = "",
                queuedFollowUps = queued,
                status = if (queued.size == 1) {
                    "Follow-up queued"
                } else {
                    "${queued.size} follow-ups queued"
                },
            )
            persist()
            return
        }
        if (state.queuedFollowUps.isNotEmpty()) {
            val queued = if (draft.isBlank()) {
                state.queuedFollowUps
            } else {
                enqueueFollowUp(state.queuedFollowUps, draft)
            }
            if (draft.isNotBlank() && queued == state.queuedFollowUps) {
                state = state.copy(status = "Follow-up queue is full")
                return
            }
            state = state.copy(draft = "", queuedFollowUps = queued)
            persist()
            sendNextFollowUp()
            return
        }
        val resultsPhoto = state.officialResultsPhoto
        val text = when {
            resultsPhoto == null -> draft
            draft.isBlank() -> "Read this official results sheet and add the clearly visible race result to this session. Official results photo attached."
            else -> "$draft\n\nOfficial results photo attached."
        }
        if (text.isBlank() || state.sendInFlight || state.stopInFlight) return
        val messageId = "phone-${UUID.randomUUID()}"
        state = state.copy(
            draft = "",
            sendInFlight = true,
            status = if (resultsPhoto == null) "Sending..." else "Uploading results photo...",
        )
        persist()
        worker.execute {
            try {
                val submitted = GatewayClient(credentials.gatewayBaseUrl).submit(
                    credentials,
                    messageId,
                    text,
                    resultsPhoto,
                )
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        state = state.copy(
                            officialResultsPhoto = null,
                            pendingJobId = submitted.jobId,
                            lastActivityAt = System.currentTimeMillis(),
                            sendInFlight = false,
                            status = "Crew Chief is thinking...",
                        )
                        persist()
                        schedulePoll(0)
                    } else {
                        sessionCollection.sessions.firstOrNull {
                            it.credentials?.sessionId == credentials.sessionId
                        }?.let { saved ->
                            val updated = saved.copy(
                                draft = if (saved.draft.trim() == draft) "" else saved.draft,
                                pendingJobId = submitted.jobId,
                                lastActivityAt = System.currentTimeMillis(),
                            )
                            storeSnapshot(updated, makeActive = false)
                            state = state.copy(sessionHistory = sessionHistoryItems(sessionCollection.sessions))
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        state = state.copy(sendInFlight = false, status = classifyError(error))
                        val restored = listOf(draft, state.draft)
                            .filter { it.isNotBlank() }
                            .joinToString("\n\n")
                            .take(4_000)
                        state = state.copy(draft = restored)
                        persist()
                    }
                }
            }
        }
    }

    private fun sendNextFollowUp() {
        val credentials = state.credentials ?: return
        val text = state.queuedFollowUps.firstOrNull() ?: return
        if (state.pendingJobId.isNotBlank() || state.sendInFlight || state.stopInFlight) return
        val messageId = "phone-${UUID.randomUUID()}"
        state = state.copy(sendInFlight = true, status = "Sending follow-up...")
        persist()
        worker.execute {
            try {
                val submitted = GatewayClient(credentials.gatewayBaseUrl)
                    .submit(credentials, messageId, text)
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        val remaining = if (state.queuedFollowUps.firstOrNull() == text) {
                            state.queuedFollowUps.drop(1)
                        } else {
                            state.queuedFollowUps
                        }
                        state = state.copy(
                            queuedFollowUps = remaining,
                            pendingJobId = submitted.jobId,
                            lastActivityAt = System.currentTimeMillis(),
                            sendInFlight = false,
                            status = if (remaining.isEmpty()) {
                                "Crew Chief is thinking..."
                            } else {
                                "Crew Chief is thinking — ${remaining.size} more queued"
                            },
                        )
                        persist()
                        schedulePoll(0)
                    } else {
                        sessionCollection.sessions.firstOrNull {
                            it.credentials?.sessionId == credentials.sessionId
                        }?.let { saved ->
                            val remaining = if (saved.queuedFollowUps.firstOrNull() == text) {
                                saved.queuedFollowUps.drop(1)
                            } else {
                                saved.queuedFollowUps
                            }
                            storeSnapshot(
                                saved.copy(
                                    queuedFollowUps = remaining,
                                    pendingJobId = submitted.jobId,
                                    lastActivityAt = System.currentTimeMillis(),
                                ),
                                makeActive = false,
                            )
                            state = state.copy(
                                sessionHistory = sessionHistoryItems(sessionCollection.sessions)
                            )
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        state = state.copy(
                            sendInFlight = false,
                            status = "Follow-up not sent: ${classifyError(error)}",
                        )
                        persist()
                    }
                }
            }
        }
    }

    private fun stopResponse() {
        val credentials = state.credentials ?: return
        val jobId = state.pendingJobId
        if (jobId.isBlank() || state.stopInFlight) return
        state = state.copy(stopInFlight = true, status = "Stopping...")
        worker.execute {
            try {
                val stopped = GatewayClient(credentials.gatewayBaseUrl)
                    .cancel(credentials, jobId)
                runOnUiThread {
                    if (
                        state.credentials?.sessionId == credentials.sessionId &&
                        state.pendingJobId == jobId
                    ) {
                        if (stopped.cancelled) {
                            val restored = (state.queuedFollowUps + state.draft)
                                .filter { it.isNotBlank() }
                                .joinToString("\n\n")
                                .take(4_000)
                            state = state.copy(
                                draft = restored,
                                pendingJobId = "",
                                queuedFollowUps = emptyList(),
                                stopInFlight = false,
                                status = if (restored.isBlank()) {
                                    "Stopped"
                                } else {
                                    "Stopped — your follow-up is ready"
                                },
                            )
                            persist()
                        } else {
                            state = state.copy(
                                stopInFlight = false,
                                status = "Answer already finished",
                            )
                            schedulePoll(0)
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        state = state.copy(
                            stopInFlight = false,
                            status = "Could not stop: ${classifyError(error)}",
                        )
                    }
                }
            }
        }
    }

    private fun pollOnce() {
        val credentials = state.credentials
        if (!foreground || credentials == null) return
        if (!networkBusy.compareAndSet(false, true)) {
            schedulePoll(500)
            return
        }
        val after = state.cursor
        val pending = state.pendingJobId
        worker.execute {
            try {
                var jobStatus = ""
                var jobError = ""
                if (pending.isNotBlank()) {
                    val job = GatewayClient(credentials.gatewayBaseUrl).job(credentials, pending)
                    jobStatus = job.status
                    jobError = job.error
                }
                val page = GatewayClient(credentials.gatewayBaseUrl).messages(credentials, after)
                runOnUiThread {
                    val terminal = jobStatus in setOf("completed", "failed", "cancelled")
                    val queuedCount = state.queuedFollowUps.size
                    val status = when (jobStatus) {
                        "queued", "thinking" -> if (queuedCount == 0) {
                            "Crew Chief is thinking..."
                        } else {
                            "Crew Chief is thinking — $queuedCount follow-up queued"
                        }
                        "completed" -> if (queuedCount == 0) "Connected" else "Sending follow-up..."
                        "cancelled" -> "Stopped"
                        "failed" -> if (jobError.isBlank()) "Crew Chief failed" else "Crew Chief failed: $jobError"
                        else -> "Connected"
                    }
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        val restoreFollowUps = jobStatus in setOf("failed", "cancelled")
                        val restoredDraft = if (restoreFollowUps) {
                            (state.queuedFollowUps + state.draft)
                                .filter { it.isNotBlank() }
                                .joinToString("\n\n")
                                .take(4_000)
                        } else {
                            state.draft
                        }
                        state = state.copy(
                            messages = mergeMessages(state.messages, page.messages),
                            cursor = maxOf(state.cursor, page.nextCursor),
                            draft = restoredDraft,
                            pendingJobId = if (terminal) "" else state.pendingJobId,
                            queuedFollowUps = if (restoreFollowUps) emptyList() else state.queuedFollowUps,
                            lastActivityAt = if (page.messages.isEmpty()) {
                                state.lastActivityAt
                            } else {
                                System.currentTimeMillis()
                            },
                            status = status,
                        )
                        persist()
                        if (jobStatus == "completed" && state.queuedFollowUps.isNotEmpty()) {
                            handler.post(::sendNextFollowUp)
                        }
                    } else {
                        sessionCollection.sessions.firstOrNull {
                            it.credentials?.sessionId == credentials.sessionId
                        }?.let { saved ->
                            val restoreFollowUps = jobStatus in setOf("failed", "cancelled")
                            val restoredDraft = if (restoreFollowUps) {
                                (saved.queuedFollowUps + saved.draft)
                                    .filter { it.isNotBlank() }
                                    .joinToString("\n\n")
                                    .take(4_000)
                            } else {
                                saved.draft
                            }
                            val updated = saved.copy(
                                messages = mergeMessages(saved.messages, page.messages),
                                cursor = maxOf(saved.cursor, page.nextCursor),
                                draft = restoredDraft,
                                pendingJobId = if (terminal) "" else saved.pendingJobId,
                                queuedFollowUps = if (restoreFollowUps) emptyList() else saved.queuedFollowUps,
                                lastActivityAt = if (page.messages.isEmpty()) {
                                    saved.lastActivityAt
                                } else {
                                    System.currentTimeMillis()
                                },
                            )
                            storeSnapshot(updated, makeActive = false)
                            state = state.copy(sessionHistory = sessionHistoryItems(sessionCollection.sessions))
                        }
                    }
                }
            } catch (error: Exception) {
                runOnUiThread {
                    if (state.credentials?.sessionId == credentials.sessionId) {
                        state = state.copy(status = classifyError(error))
                    }
                }
            } finally {
                networkBusy.set(false)
                runOnUiThread { schedulePoll(2_000) }
            }
        }
    }

    private fun clearConversation() {
        val credentials = state.credentials ?: return
        if (credentials.offlineDemo) {
            val replacement = offlineDemoSnapshot()
            storeSnapshot(replacement, makeActive = true)
            state = stateFromSnapshot(replacement, "Offline demo reset locally")
            return
        }
        if (!networkBusy.compareAndSet(false, true)) return
        state = state.copy(confirmClear = false, status = "Creating transcript backup…")
        worker.execute {
            try {
                val cleared = GatewayClient(credentials.gatewayBaseUrl).clear(credentials)
                runOnUiThread {
                    state = state.copy(
                        messages = emptyList(),
                        cursor = 0,
                        pendingJobId = "",
                        queuedFollowUps = emptyList(),
                        stopInFlight = false,
                        lastActivityAt = System.currentTimeMillis(),
                        status = "Cleared after backup: ${cleared.backupName}",
                    )
                    persist()
                }
            } catch (error: Exception) {
                runOnUiThread { state = state.copy(status = classifyError(error)) }
            } finally {
                networkBusy.set(false)
            }
        }
    }

    private fun unpair() {
        val credentials = state.credentials ?: return
        if (credentials.offlineDemo) {
            secureStore.remove(credentials.sessionId)
            sessionCollection = removeSession(sessionCollection, credentials.sessionId)
            val next = activeSession(sessionCollection)
            state = stateFromSnapshot(next, if (next == null) "Offline demo closed" else "Connecting...")
            schedulePoll(0)
            return
        }
        if (!networkBusy.compareAndSet(false, true)) return
        state = state.copy(status = "Unpairing…")
        worker.execute {
            try {
                GatewayClient(credentials.gatewayBaseUrl).unpair(credentials)
                runOnUiThread {
                    secureStore.remove(credentials.sessionId)
                    sessionCollection = removeSession(sessionCollection, credentials.sessionId)
                    val next = activeSession(sessionCollection)
                    next?.credentials?.sessionId?.let(secureStore::setActive)
                    state = stateFromSnapshot(
                        next,
                        if (next == null) "Phone unpaired" else "Connecting...",
                    )
                    schedulePoll(0)
                }
            } catch (error: Exception) {
                runOnUiThread { state = state.copy(status = classifyError(error)) }
            } finally {
                networkBusy.set(false)
            }
        }
    }

    private fun schedulePoll(delayMillis: Long) {
        handler.removeCallbacks(pollRunnable)
        if (
            foreground && state.credentials != null &&
            state.credentials?.offlineDemo != true && !state.pairingMode
        ) {
            handler.postDelayed(pollRunnable, delayMillis)
        }
    }

    private fun persist() {
        val snapshot = currentSnapshot() ?: return
        storeSnapshot(snapshot, makeActive = true)
        val history = sessionHistoryItems(sessionCollection.sessions)
        if (state.sessionHistory != history) state = state.copy(sessionHistory = history)
    }

    private fun classifyError(error: Throwable): String {
        val message = error.message.orEmpty()
        val lower = message.lowercase()
        return when {
            lower.contains("expired") || lower.contains("already used") -> "Pairing code expired — make a new one in Viewer"
            lower.contains("revoked") || lower.contains("unauthorized") || lower.contains("invalid token") -> "Pairing revoked — pair again from Viewer"
            error is IOException || lower.contains("timeout") || lower.contains("connect") -> "Offline — open Tailscale and try again"
            message.isNotBlank() -> "Failed: $message"
            else -> "Failed"
        }
    }
}

@Composable
private fun RaceBoxTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            primary = Color(0xFF55D6BE),
            secondary = Color(0xFF65AFFF),
            background = Color(0xFF0A0F14),
            surface = Color(0xFF111A23),
            onBackground = Color(0xFFEAF2F8),
            onSurface = Color(0xFFEAF2F8),
        ),
        content = content,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CompanionApp(
    state: AppUiState,
    onDraftChanged: (String) -> Unit,
    onManualCodeChanged: (String) -> Unit,
    onGatewayChanged: (String) -> Unit,
    onPair: () -> Unit,
    onScan: () -> Unit,
    onOfflineDemo: () -> Unit,
    onConfirmGateway: () -> Unit,
    onCancelGateway: () -> Unit,
    onSend: () -> Unit,
    onStop: () -> Unit,
    onShowPhotoSource: () -> Unit,
    onHidePhotoSource: () -> Unit,
    onTakeResultsPhoto: () -> Unit,
    onChooseResultsPhoto: () -> Unit,
    onRemoveResultsPhoto: () -> Unit,
    onShowHistory: () -> Unit,
    onHideHistory: () -> Unit,
    onSelectSession: (String) -> Unit,
    onPairAnother: () -> Unit,
    onCancelPairing: () -> Unit,
    onShowInfo: () -> Unit,
    onHideInfo: () -> Unit,
    onRequestClear: () -> Unit,
    onCancelClear: () -> Unit,
    onConfirmClear: () -> Unit,
    onUnpair: () -> Unit,
) {
    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        if (state.credentials == null || state.pairingMode) {
            PairingScreen(
                state,
                onManualCodeChanged,
                onGatewayChanged,
                onPair,
                onScan,
                onOfflineDemo,
                onShowInfo,
                onCancelPairing,
            )
        } else {
            ChatScreen(
                state,
                onDraftChanged,
                onSend,
                onStop,
                onShowPhotoSource,
                onRemoveResultsPhoto,
                onShowHistory,
                onShowInfo,
            )
        }
    }
    if (state.showHistory) {
        ModalBottomSheet(onDismissRequest = onHideHistory) {
            HistorySheet(
                sessions = state.sessionHistory,
                activeSessionId = state.credentials?.sessionId.orEmpty(),
                onSelectSession = onSelectSession,
                onPairAnother = onPairAnother,
            )
        }
    }
    if (state.showInfo) {
        ModalBottomSheet(onDismissRequest = onHideInfo) {
            InformationSheet(state, onRequestClear, onUnpair)
        }
    }
    if (state.confirmClear) {
        AlertDialog(
            onDismissRequest = onCancelClear,
            title = { Text("Clear this conversation?") },
            text = {
                Text(
                    if (state.credentials?.offlineDemo == true) {
                        "Reset the local scripted conversation? No server is involved."
                    } else {
                        "The gateway creates a timestamped transcript backup before anything is removed."
                    }
                )
            },
            confirmButton = {
                TextButton(onClick = onConfirmClear) {
                    Text(if (state.credentials?.offlineDemo == true) "Reset demo" else "Back up and clear")
                }
            },
            dismissButton = { TextButton(onClick = onCancelClear) { Text("Cancel") } },
        )
    }
    if (state.confirmGateway) {
        AlertDialog(
            onDismissRequest = onCancelGateway,
            title = { Text("Connect to this gateway?") },
            text = {
                Text(
                    "This host is not in your saved sessions:\n\n${state.gatewayUrl}\n\n" +
                        "Continue only if it is your RaceBox gateway or one you trust."
                )
            },
            confirmButton = { TextButton(onClick = onConfirmGateway) { Text("Connect") } },
            dismissButton = { TextButton(onClick = onCancelGateway) { Text("Cancel") } },
        )
    }
    if (state.showPhotoSource) {
        AlertDialog(
            onDismissRequest = onHidePhotoSource,
            title = { Text("Official results") },
            text = {
                Text("Take a clear, straight-on photo of the official result sheet, or choose one already on this phone.")
            },
            confirmButton = {
                TextButton(onClick = onTakeResultsPhoto) { Text("Take photo") }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = onChooseResultsPhoto) { Text("Choose photo") }
                    TextButton(onClick = onHidePhotoSource) { Text("Cancel") }
                }
            },
        )
    }
}

@Composable
private fun PairingScreen(
    state: AppUiState,
    onManualCodeChanged: (String) -> Unit,
    onGatewayChanged: (String) -> Unit,
    onPair: () -> Unit,
    onScan: () -> Unit,
    onOfflineDemo: () -> Unit,
    onShowInfo: () -> Unit,
    onCancelPairing: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("RaceBox Crew Chief", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(8.dp))
        Text("Try it instantly, or connect to your own Viewer gateway.", color = Color(0xFFAAC0D0))
        Spacer(Modifier.height(20.dp))
        OutlinedButton(onClick = onOfflineDemo, modifier = Modifier.fillMaxWidth()) {
            Text("Try offline demo")
        }
        Text("Scripted Richmond answers — no network", color = Color(0xFFAAC0D0))
        Spacer(Modifier.height(16.dp))
        Button(onClick = onScan, modifier = Modifier.fillMaxWidth()) { Text("Scan QR code") }
        Spacer(Modifier.height(12.dp))
        Text("or enter your gateway and six-digit code")
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = state.gatewayUrl,
            onValueChange = onGatewayChanged,
            label = { Text("My gateway URL") },
            placeholder = { Text("https://gateway.example") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = state.manualCode,
            onValueChange = onManualCodeChanged,
            label = { Text("Pairing code") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { onPair() }),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(12.dp))
        Button(
            onClick = onPair,
            enabled = state.manualCode.length == 6 && state.gatewayUrl.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Connect to my gateway")
        }
        Spacer(Modifier.height(18.dp))
        StatusLine(state.status)
        TextButton(onClick = onShowInfo) { Text("Connection information") }
        if (state.credentials != null) {
            TextButton(onClick = onCancelPairing) { Text("Back to current chat") }
        }
    }
}

@Composable
private fun ChatScreen(
    state: AppUiState,
    onDraftChanged: (String) -> Unit,
    onSend: () -> Unit,
    onStop: () -> Unit,
    onShowPhotoSource: () -> Unit,
    onRemoveResultsPhoto: () -> Unit,
    onShowHistory: () -> Unit,
    onShowInfo: () -> Unit,
) {
    val listState = rememberLazyListState()
    LaunchedEffect(state.messages.size) {
        if (state.messages.isNotEmpty()) listState.animateScrollToItem(state.messages.lastIndex)
    }
    Scaffold(
        modifier = Modifier.fillMaxSize(),
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            Row(
                modifier = Modifier.fillMaxWidth().statusBarsPadding()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextButton(onClick = onShowHistory) { Text("Chats") }
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        state.credentials?.title ?: "RaceBox session",
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        fontWeight = FontWeight.Bold,
                    )
                    Text("Crew Chief", style = MaterialTheme.typography.labelMedium, color = Color(0xFFAAC0D0))
                }
                TextButton(onClick = onShowInfo) { Text("More") }
            }
        },
        bottomBar = {
            Column(
                modifier = Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface)
                    .navigationBarsPadding().padding(12.dp),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        StatusLine(state.status)
                        if (state.queuedFollowUps.isNotEmpty()) {
                            Text(
                                if (state.queuedFollowUps.size == 1) {
                                    "1 follow-up waiting"
                                } else {
                                    "${state.queuedFollowUps.size} follow-ups waiting"
                                },
                                color = Color(0xFF65AFFF),
                                style = MaterialTheme.typography.labelMedium,
                            )
                        }
                    }
                    if (state.pendingJobId.isNotBlank()) {
                        OutlinedButton(
                            onClick = onStop,
                            enabled = !state.stopInFlight,
                        ) {
                            Text(if (state.stopInFlight) "Stopping" else "Stop")
                        }
                    }
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(
                        onClick = onShowPhotoSource,
                        enabled = !state.photoPreparing && !state.sendInFlight && state.pendingJobId.isBlank(),
                    ) {
                        Text(if (state.officialResultsPhoto == null) "Results photo" else "Replace photo")
                    }
                    state.officialResultsPhoto?.let { photo ->
                        Text(
                            "Ready (${(photo.bytes.size + 1023) / 1024} KB)",
                            modifier = Modifier.weight(1f),
                            color = Color(0xFF55D6BE),
                            style = MaterialTheme.typography.labelMedium,
                        )
                        TextButton(onClick = onRemoveResultsPhoto) { Text("Remove") }
                    }
                }
                Row(verticalAlignment = Alignment.Bottom) {
                    OutlinedTextField(
                        value = state.draft,
                        onValueChange = onDraftChanged,
                        placeholder = { Text("Ask Crew Chief") },
                        maxLines = 5,
                        modifier = Modifier.weight(1f),
                    )
                    Button(
                        onClick = onSend,
                        enabled = (
                            state.draft.isNotBlank() ||
                                (state.pendingJobId.isBlank() && state.officialResultsPhoto != null) ||
                                (state.pendingJobId.isBlank() && state.queuedFollowUps.isNotEmpty())
                            ) && !state.photoPreparing && !state.sendInFlight && !state.stopInFlight,
                        modifier = Modifier.padding(start = 8.dp),
                    ) {
                        Text(if (state.pendingJobId.isBlank()) "Send" else "Follow up")
                    }
                }
            }
        },
    ) { padding ->
        if (state.messages.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
                Text("Ask about the active run.", color = Color(0xFFAAC0D0))
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize().padding(padding).padding(horizontal = 12.dp),
                contentPadding = PaddingValues(vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp, alignment = Alignment.Bottom),
            ) {
                items(state.messages, key = { it.id }) { message -> MessageBubble(message) }
            }
        }
    }
}

@Composable
private fun HistorySheet(
    sessions: List<SessionHistoryItem>,
    activeSessionId: String,
    onSelectSession: (String) -> Unit,
    onPairAnother: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp).padding(bottom = 32.dp),
    ) {
        Text("Chats", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(12.dp))
        Button(onClick = onPairAnother, modifier = Modifier.fillMaxWidth()) {
            Text("Pair another session")
        }
        Spacer(Modifier.height(12.dp))
        LazyColumn(
            modifier = Modifier.fillMaxWidth().heightIn(max = 520.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(sessions, key = { it.sessionId }) { session ->
                val active = session.sessionId == activeSessionId
                Card(
                    modifier = Modifier.fillMaxWidth().clickable {
                        onSelectSession(session.sessionId)
                    },
                    shape = RoundedCornerShape(14.dp),
                ) {
                    Column(
                        modifier = Modifier.fillMaxWidth()
                            .background(if (active) Color(0xFF174A46) else Color(0xFF182530))
                            .padding(14.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                session.title,
                                modifier = Modifier.weight(1f),
                                fontWeight = FontWeight.Bold,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            if (active) Text("Current", color = Color(0xFF55D6BE))
                        }
                        Spacer(Modifier.height(3.dp))
                        Text(
                            if (session.messageCount == 1) "1 message" else "${session.messageCount} messages",
                            style = MaterialTheme.typography.labelMedium,
                            color = Color(0xFFAAC0D0),
                        )
                        if (session.preview.isNotBlank()) {
                            Spacer(Modifier.height(4.dp))
                            Text(
                                session.preview,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis,
                                color = Color(0xFFD7E2EA),
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun MessageBubble(message: ChatMessage) {
    val assistant = message.role == "assistant"
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = if (assistant) Arrangement.Start else Arrangement.End,
    ) {
        Card(
            modifier = Modifier.widthIn(max = 340.dp),
            shape = RoundedCornerShape(16.dp),
        ) {
            Column(
                modifier = Modifier.background(if (assistant) Color(0xFF182530) else Color(0xFF174A46))
                    .padding(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text(message.content)
                if (message.origin.isNotBlank()) {
                    Spacer(Modifier.height(4.dp))
                    Text(
                        if (message.origin == "phone") "Phone" else "Viewer",
                        style = MaterialTheme.typography.labelSmall,
                        color = Color(0xFFAAC0D0),
                    )
                }
            }
        }
    }
}

@Composable
private fun StatusLine(status: String) {
    Text(
        status,
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        style = MaterialTheme.typography.labelMedium,
        color = if (status == "Connected") Color(0xFF55D6BE) else Color(0xFFFFC857),
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
    )
}

@Composable
private fun InformationSheet(
    state: AppUiState,
    onRequestClear: () -> Unit,
    onUnpair: () -> Unit,
) {
    Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 24.dp).padding(bottom = 36.dp)) {
        Text("Connection", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(16.dp))
        Text(
            "Gateway: " + when {
                state.credentials?.offlineDemo == true -> "Offline demo (no network)"
                state.credentials != null -> state.credentials.gatewayBaseUrl
                else -> "Not connected"
            }
        )
        Text("Connection contract: $CONNECTION_CONTRACT")
        Text("Health contract: $HEALTH_CONTRACT")
        Text("Prompt: $PROMPT_REVISION")
        Text("Companion contract: $COMPANION_CONTRACT")
        state.credentials?.let {
            Text("Session: ${it.sessionId}")
            Text("Saved chats: ${state.sessionHistory.size}")
            Text("Messages: ${state.messages.size}  |  Cursor: ${state.cursor}")
            Spacer(Modifier.height(20.dp))
            OutlinedButton(onClick = onRequestClear, modifier = Modifier.fillMaxWidth()) {
                Text(if (it.offlineDemo) "Reset offline demo" else "Back up and clear conversation")
            }
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = onUnpair, modifier = Modifier.fillMaxWidth()) {
                Text(if (it.offlineDemo) "Close offline demo" else "Unpair this session from phone")
            }
        } ?: Text("Not paired. Live connections require your gateway; the offline demo does not.")
        Spacer(Modifier.size(8.dp))
    }
}
