package com.alexpate.raceboxcrewchief

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.net.Uri
import android.os.Build
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.security.MessageDigest
import kotlin.math.max
import kotlin.math.roundToInt

private const val MAX_PHOTO_EDGE = 2_000
const val MAX_OFFICIAL_RESULTS_PHOTO_BYTES = 3 * 1024 * 1024

data class OfficialResultsPhoto(
    val mimeType: String,
    val sha256: String,
    val bytes: ByteArray,
)

fun prepareOfficialResultsPhoto(context: Context, uri: Uri): OfficialResultsPhoto {
    val bitmap = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        decodeWithImageDecoder(context, uri)
    } else {
        decodeWithBitmapFactory(context, uri)
    }
    try {
        var quality = 88
        var encoded = encodeJpeg(bitmap, quality)
        while (encoded.size > MAX_OFFICIAL_RESULTS_PHOTO_BYTES && quality > 55) {
            quality -= 8
            encoded = encodeJpeg(bitmap, quality)
        }
        if (encoded.size > MAX_OFFICIAL_RESULTS_PHOTO_BYTES) {
            throw IOException("Photo is still too large after resizing. Move closer and retake it.")
        }
        return OfficialResultsPhoto(
            mimeType = "image/jpeg",
            sha256 = MessageDigest.getInstance("SHA-256")
                .digest(encoded)
                .joinToString("") { byte -> "%02x".format(byte) },
            bytes = encoded,
        )
    } finally {
        bitmap.recycle()
    }
}

private fun encodeJpeg(bitmap: Bitmap, quality: Int): ByteArray {
    val output = ByteArrayOutputStream()
    if (!bitmap.compress(Bitmap.CompressFormat.JPEG, quality, output)) {
        throw IOException("Could not prepare the results photo")
    }
    return output.toByteArray()
}

@androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
private fun decodeWithImageDecoder(context: Context, uri: Uri): Bitmap {
    val source = ImageDecoder.createSource(context.contentResolver, uri)
    return ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
        val longest = max(info.size.width, info.size.height)
        if (longest > MAX_PHOTO_EDGE) {
            val scale = MAX_PHOTO_EDGE.toDouble() / longest.toDouble()
            decoder.setTargetSize(
                (info.size.width * scale).roundToInt().coerceAtLeast(1),
                (info.size.height * scale).roundToInt().coerceAtLeast(1),
            )
        }
        decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
    }
}

private fun decodeWithBitmapFactory(context: Context, uri: Uri): Bitmap {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    context.contentResolver.openInputStream(uri)?.use { stream ->
        BitmapFactory.decodeStream(stream, null, bounds)
    } ?: throw IOException("Could not open the results photo")
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
        throw IOException("The selected file is not a readable photo")
    }
    var sampleSize = 1
    while (max(bounds.outWidth, bounds.outHeight) / sampleSize > MAX_PHOTO_EDGE) {
        sampleSize *= 2
    }
    val options = BitmapFactory.Options().apply { inSampleSize = sampleSize }
    return context.contentResolver.openInputStream(uri)?.use { stream ->
        BitmapFactory.decodeStream(stream, null, options)
    } ?: throw IOException("Could not decode the results photo")
}
