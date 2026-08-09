package com.alexpate.raceboxcrewchief

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class CompanionLogicTest {
    @Test
    fun acceptsUserSuppliedSafePairingLink() {
        assertEquals(
            PairingLink("http://100.64.10.20:18804", "123456"),
            parsePairingLink("raceboxcc://pair?gateway=http%3A%2F%2F100.64.10.20%3A18804&code=123456"),
        )
        assertEquals(
            PairingLink("https://gateway.example.test", "654321"),
            parsePairingLink("raceboxcc://pair?gateway=https%3A%2F%2Fgateway.example.test&code=654321"),
        )
    }

    @Test
    fun enforcesGatewayUrlPolicyAndMalformedCodes() {
        assertNull(parsePairingLink("raceboxcc://pair?gateway=http%3A%2F%2F192.168.1.20%3A18804&code=123456"))
        assertNull(parsePairingLink("raceboxcc://pair?gateway=http%3A%2F%2F100.64.10.20%3A18804&code=123"))
        assertNull(parsePairingLink("https://gateway.example.test/pair?code=123456"))
        assertEquals(false, validateGatewayUrl("http://100.128.0.1:18804").valid)
        assertEquals(false, validateGatewayUrl("https://user:secret@gateway.example.test").valid)
    }

    @Test
    fun messageMergeIsStableAndDeduplicatesById() {
        val original = ChatMessage(1, "m1", "user", "old", "viewer", 10)
        val replacement = original.copy(content = "updated")
        val second = ChatMessage(2, "m2", "assistant", "answer", "crew-chief", 20)

        assertEquals(listOf(replacement, second), mergeMessages(listOf(original), listOf(second, replacement)))
    }

    @Test
    fun upsertKeepsOtherSessionAndReplacesOnlyMatchingSession() {
        val first = snapshot("s1", "First", 10)
        val second = snapshot("s2", "Second", 20)
        val updatedFirst = first.copy(draft = "saved draft", lastActivityAt = 30)

        assertEquals(
            listOf(second, updatedFirst),
            upsertSession(listOf(first, second), updatedFirst),
        )
    }

    @Test
    fun historyIsNewestFirstAndActiveSelectionSurvives() {
        val older = snapshot("s1", "Older", 10)
        val newer = snapshot("s2", "Newer", 20)
        val collection = SessionCollection("s1", listOf(older, newer))

        assertEquals(listOf("s2", "s1"), sessionHistoryItems(collection.sessions).map { it.sessionId })
        assertSame(older, activeSession(collection))
    }

    @Test
    fun removingActiveSessionSelectsNewestRemainingChat() {
        val oldest = snapshot("s1", "Oldest", 10)
        val active = snapshot("s2", "Active", 20)
        val newest = snapshot("s3", "Newest", 30)

        val remaining = removeSession(SessionCollection("s2", listOf(oldest, active, newest)), "s2")

        assertEquals("s3", remaining.activeSessionId)
        assertEquals(listOf(oldest, newest), remaining.sessions)
    }

    @Test
    fun followUpsAreTrimmedOrderedAndBounded() {
        var queued = enqueueFollowUp(emptyList(), "  first correction  ")
        queued = enqueueFollowUp(queued, "second detail")
        assertEquals(listOf("first correction", "second detail"), queued)
        assertEquals(queued, enqueueFollowUp(queued, "   "))

        repeat(MAX_QUEUED_FOLLOW_UPS) { index ->
            queued = enqueueFollowUp(queued, "extra $index")
        }
        assertEquals(MAX_QUEUED_FOLLOW_UPS, queued.size)
        assertEquals(queued, enqueueFollowUp(queued, "too many"))
    }

    @Test
    fun offlineDemoIsClearlyLabelledAndNeedsNoConnection() {
        val snapshot = offlineDemoSnapshot(1234)
        assertEquals(true, snapshot.credentials?.offlineDemo)
        assertEquals("", snapshot.credentials?.gatewayBaseUrl)
        assertEquals(true, snapshot.messages.single().content.startsWith("OFFLINE DEMO"))
        assertEquals(true, offlineDemoAnswer("Is it reliable?").startsWith("OFFLINE DEMO"))
    }

    @Test
    fun legacySessionMigrationNeverRecreatesADeveloperGateway() {
        val legacy = migrateStoredConnection(3, null, offlineDemo = false)
        assertEquals("", legacy.gatewayBaseUrl)
        assertEquals(true, legacy.requiresRepair)
        val current = migrateStoredConnection(
            4, "https://gateway.example.test/", offlineDemo = false
        )
        assertEquals("https://gateway.example.test", current.gatewayBaseUrl)
        assertEquals(false, current.requiresRepair)
    }

    private fun snapshot(sessionId: String, title: String, activity: Long): SessionSnapshot {
        return SessionSnapshot(
            credentials = SessionCredentials(
                sessionId, "token-$sessionId", "pair-$sessionId", title,
                "https://gateway.example.test",
            ),
            messages = emptyList(),
            draft = "",
            cursor = 0,
            pendingJobId = "",
            lastActivityAt = activity,
        )
    }
}
