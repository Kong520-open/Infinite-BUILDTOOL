package com.vdl.kong520;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Locale;

/** JNI surface for the four building tools only. */
public final class TpModule {
    private static final String TAG = "BuildToolsTp";

    private TpModule() {}

    public static native String getWorldId();

    public static native boolean startBuildImport(String sourcePath, String spoolDirectory, String jobId,
                                                   int baseX, int baseY, int baseZ, int blocksPerSecond,
                                                   boolean clearExisting, boolean placeDenyLayer,
                                                   boolean verifyAfterImport, int verificationPrecision,
                                                   int simulationChunkRange, String worldId, int dimensionId,
                                                   boolean suppressCommandFeedback);
    public static native boolean startPixelArtImport(String sourcePath, String spoolDirectory, String jobId,
                                                      int baseX, int baseY, int baseZ, int targetWidth,
                                                      int blocksPerSecond, boolean clearExisting,
                                                      boolean placeDenyLayer, boolean verifyAfterImport,
                                                      int verificationPrecision, int simulationChunkRange,
                                                      String worldId, int dimensionId,
                                                      boolean createMapsAfterImport,
                                                      boolean suppressCommandFeedback);
    public static native boolean restoreBuildImport(String spoolDirectory, String worldId, int dimensionId);
    public static native boolean undoLastBuildImport(String storageDirectory, String spoolDirectory,
                                                       String worldId, int dimensionId);
    public static native boolean discardBuildImportUndoClaim(String storageDirectory,
                                                              String spoolDirectory);
    public static native void pauseBuildImport();
    public static native boolean resumeBuildImport(String worldId, int dimensionId);
    public static native void cancelBuildImport();
    public static native int getBuildImportState();
    public static native String getBuildImportStatus();
    public static native long getBuildImportTotalBlocks();
    public static native long getBuildImportImportedBlocks();
    public static native int[] getBuildImportPlayerBlockPosition();

    public static native int[] getBuildExportPlayerBlockPosition();
    public static native boolean startBuildExport(String outputPath,
                                                   int firstX, int firstY, int firstZ,
                                                   int secondX, int secondY, int secondZ,
                                                   int teleportMode, String worldId, int dimensionId,
                                                   boolean replaceCheckpoint, int simulationChunkRange,
                                                   boolean exportContainerItems);
    public static native boolean hasBuildExportCheckpoint(String outputPath);
    public static native boolean resumeBuildExport(String outputPath, int teleportMode,
                                                    String worldId, int dimensionId);
    public static native boolean discardBuildExportCheckpoint(String outputPath);
    public static native void cancelBuildExport();
    public static native int getBuildExportState();
    public static native String getBuildExportStatus();
    public static native long getBuildExportTotalBlocks();
    public static native long getBuildExportProcessedBlocks();
    public static native boolean isBuildExportTeleportAllowed();
    public static native int getBuildExportTeleportMode();
    public static native boolean requestBuildExportNextRegion();
    public static native int[] getBuildExportTravelTarget();

    public static native boolean loadBuildProjection(String sourcePath, String workDirectory,
                                                      int baseX, int baseY, int baseZ,
                                                      int rotationDegrees, int pixelArtWidth);
    public static native void clearBuildProjection();
    public static native String getBuildProjectionStatus();
    /**
     * Paged summary of every material in the complete loaded projection.
     * The native response is a tab-separated v1 header followed by at most
     * {@code pageSize} material rows; see BuildProjectionMaterialPreviewOverlay.
     */
    public static native String getBuildProjectionMaterialSummaryPage(int pageIndex, int pageSize);
    public static native void setBuildProjectionEnabled(boolean enabled);
    public static native void setBuildProjectionReachabilityPreviewEnabled(boolean enabled);
    public static native boolean setBuildProjectionOutlineEnabled(boolean enabled);
    public static native void setBuildProjectionAlpha(float alpha);
    public static native void setBuildProjectionRange(int rangeChunks);
    public static native void setBuildProjectionLayerFilter(int mode, boolean worldSpace,
                                                             int minimumY, int maximumY);
    public static native void setBuildProjectionPrinterEnabled(boolean enabled);
    public static native boolean isBuildProjectionPrinterEnabled();
    public static native void setBuildProjectionPrinterRate(int blocksPerSecond);
    public static native int getBuildProjectionPrinterRate();
    public static native int getBuildProjectionPrinterState();
    public static native String getBuildProjectionPrinterStatus();
    public static native long getBuildProjectionPrinterTotalBlocks();
    public static native long getBuildProjectionPrinterPlacedBlocks();
    public static native long getBuildProjectionPrinterSkippedBlocks();
    public static native String getBuildProjectionTextureRequests();
    public static native boolean installBuildProjectionTexturePack(String[] materialKeys,
                                                                     short[] faceLayers, int tileSize,
                                                                     int layerCount, byte[] layerPixels);

    /** Decode JPEG with bounded sampling before the native pixel-art conversion. */
    public static String preparePixelArtSource(String sourcePath, String workDirectory, int targetWidth) {
        if (sourcePath == null || workDirectory == null) return null;
        final String lower = sourcePath.toLowerCase(Locale.ROOT);
        if (!(lower.endsWith(".jpg") || lower.endsWith(".jpeg"))) return sourcePath;

        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(sourcePath, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || targetWidth <= 0) return null;

        final int orientation = readJpegOrientation(sourcePath);
        final boolean swapsAxes = orientation == ExifInterface.ORIENTATION_TRANSPOSE
                || orientation == ExifInterface.ORIENTATION_ROTATE_90
                || orientation == ExifInterface.ORIENTATION_TRANSVERSE
                || orientation == ExifInterface.ORIENTATION_ROTATE_270;
        final long sourceOrientedWidth = swapsAxes ? bounds.outHeight : bounds.outWidth;
        final long heapBudgetBytes = Math.min(64L * 1024L * 1024L,
                Math.max(16L * 1024L * 1024L, Runtime.getRuntime().maxMemory() / 8L));
        final long maxDecodedPixels = Math.max(1L, heapBudgetBytes / 4L);
        int sample = 1;
        while (sample <= (1 << 29)) {
            final long sampledWidth = Math.max(1L, (bounds.outWidth + (long) sample - 1L) / sample);
            final long sampledHeight = Math.max(1L, (bounds.outHeight + (long) sample - 1L) / sample);
            final long sampledPixels = sampledWidth > Long.MAX_VALUE / sampledHeight
                    ? Long.MAX_VALUE : sampledWidth * sampledHeight;
            if (sampledWidth == 1L && sampledHeight == 1L) break;
            final int nextSample = sample << 1;
            final long nextOrientedWidth = Math.max(1L,
                    (sourceOrientedWidth + nextSample - 1L) / nextSample);
            if (sampledPixels <= maxDecodedPixels && nextOrientedWidth < targetWidth) break;
            sample = nextSample;
        }

        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = sample;
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap bitmap = BitmapFactory.decodeFile(sourcePath, options);
        if (bitmap == null) return null;
        File output = new File(workDirectory,
                ".pixelart_" + Integer.toHexString(sourcePath.hashCode()) + ".png");
        try {
            Bitmap oriented = applyJpegOrientation(bitmap, orientation);
            if (oriented != bitmap) {
                bitmap.recycle();
                bitmap = oriented;
            }
            File parent = output.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
                return null;
            }
            if (output.exists() && !output.delete()) return null;
            try (FileOutputStream stream = new FileOutputStream(output)) {
                if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)) return null;
                stream.flush();
            }
            return output.getAbsolutePath();
        } catch (IOException | RuntimeException error) {
            if (output.exists()) output.delete();
            Log.e(TAG, "Cannot prepare JPEG pixel art", error);
            return null;
        } finally {
            bitmap.recycle();
        }
    }

    private static int readJpegOrientation(String sourcePath) {
        try {
            return new ExifInterface(sourcePath).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException | RuntimeException ignored) {
            return ExifInterface.ORIENTATION_NORMAL;
        }
    }

    private static Bitmap applyJpegOrientation(Bitmap source, int orientation) {
        Matrix matrix = new Matrix();
        switch (orientation) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL:
                matrix.setScale(-1.0f, 1.0f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_180:
                matrix.setRotate(180.0f);
                break;
            case ExifInterface.ORIENTATION_FLIP_VERTICAL:
                matrix.setRotate(180.0f);
                matrix.postScale(-1.0f, 1.0f);
                break;
            case ExifInterface.ORIENTATION_TRANSPOSE:
                matrix.setRotate(90.0f);
                matrix.postScale(-1.0f, 1.0f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_90:
                matrix.setRotate(90.0f);
                break;
            case ExifInterface.ORIENTATION_TRANSVERSE:
                matrix.setRotate(-90.0f);
                matrix.postScale(-1.0f, 1.0f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_270:
                matrix.setRotate(-90.0f);
                break;
            default:
                return source;
        }
        return Bitmap.createBitmap(source, 0, 0, source.getWidth(), source.getHeight(), matrix, true);
    }
}
