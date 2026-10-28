package space.darkdomain.campusnet

import org.junit.Assert.*
import org.junit.Test
import javax.crypto.KeyGenerator

class CredentialCipherTest {
    @Test fun roundTripAndRejectTampering() {
        val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        val first = encryptCredentials(key, "example user", "example password")
        val second = encryptCredentials(key, "example user", "example password")
        assertNotEquals(first, second)
        assertEquals(Credentials("example user", "example password"), decryptCredentials(key, first))
        val bytes = java.util.Base64.getDecoder().decode(first)
        bytes[bytes.lastIndex] = (bytes.last().toInt() xor 1).toByte()
        assertThrows(Exception::class.java) { decryptCredentials(key, java.util.Base64.getEncoder().encodeToString(bytes)) }
    }
}
