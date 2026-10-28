package space.darkdomain.campusnet

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

data class Credentials(val user: String, val password: String)

fun encryptCredentials(key: SecretKey, user: String, password: String): String {
    val payload = ByteArrayOutputStream().also { output ->
        DataOutputStream(output).use { it.writeUTF(user); it.writeUTF(password) }
    }.toByteArray()
    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(Cipher.ENCRYPT_MODE, key)
    return Base64.getEncoder().encodeToString(cipher.iv + cipher.doFinal(payload))
}

fun decryptCredentials(key: SecretKey, value: String): Credentials {
    val data = Base64.getDecoder().decode(value)
    require(data.size >= 28) { "Invalid credential data" }
    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, data.copyOfRange(0, 12)))
    return DataInputStream(ByteArrayInputStream(cipher.doFinal(data.copyOfRange(12, data.size)))).use {
        val credentials = Credentials(it.readUTF(), it.readUTF())
        require(it.available() == 0)
        credentials
    }
}
