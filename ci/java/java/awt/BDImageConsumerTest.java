/*
 * A BD-J image loaded from a file stays complete (patches/libbluray/0004).
 *
 * Without the patch, libbluray's BDImageConsumer stays a consumer of its
 * producer after STATICIMAGEDONE, and the JDK's image fetcher reports
 * IMAGEERROR to it as the fetch ends. A MediaTracker that checks the image
 * after that sees ERRORED: a disc's Java menu that loads its images this way
 * fails to start whenever its check comes after the fetcher's (an intermittent
 * "io exception" from the application's startXlet).
 *
 * Run with the AWT jar patched into java.desktop (ci/smoke.sh):
 *   java --patch-module java.desktop=<libbluray-awt-j2se.jar>:<classes> java.awt.BDImageConsumerTest
 */
package java.awt;

import java.awt.image.ImageObserver;
import java.io.File;
import java.io.FileOutputStream;

public class BDImageConsumerTest {
    /* A 64x32 RGBA PNG. */
    private static final byte[] PNG = hex(
        "89504e470d0a1a0a0000000d4948445200000040000000200806000000a29d7e8400000042494441"
        + "5478daedd03101000004003051441145ff12f460c70a2cb27a3e0b01020408102040800001020408"
        + "1020408000010204081020408000010204081020e0be05d1cff8c4340b12300000000049454e44ae"
        + "426082");

    private static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            b[i] = (byte)Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return b;
    }

    public static void main(String[] args) throws Exception {
        File png = File.createTempFile("bdimage", ".png");
        png.deleteOnExit();
        FileOutputStream out = new FileOutputStream(png);
        out.write(PNG);
        out.close();
        for (int i = 0; i < 20; i++) {
            BDImageConsumer image = new BDImageConsumer(new sun.awt.image.FileImageSource(png.getPath()));
            final Object lock = new Object();
            final int[] seen = new int[1];
            ImageObserver observer = new ImageObserver() {
                public boolean imageUpdate(Image img, int flags, int x, int y, int w, int h) {
                    synchronized (lock) {
                        seen[0] |= flags;
                        lock.notifyAll();
                    }
                    return (flags & (ALLBITS | ERROR | ABORT)) == 0;
                }
            };
            image.prepareImage(observer);
            long end = System.currentTimeMillis() + 10000;
            synchronized (lock) {
                while ((seen[0] & (ImageObserver.ALLBITS | ImageObserver.ERROR | ImageObserver.ABORT)) == 0
                       && System.currentTimeMillis() < end) {
                    lock.wait(100);
                }
            }
            // Long enough for the fetch to end and its clean-up to run.
            Thread.sleep(200);
            int status = image.checkImage(null);
            if ((status & ImageObserver.ALLBITS) == 0 || (status & (ImageObserver.ERROR | ImageObserver.ABORT)) != 0) {
                throw new AssertionError("load " + i + ": status " + status + " (observer saw " + seen[0] + ")");
            }
            if (image.getWidth(null) != 64 || image.getHeight(null) != 32) {
                throw new AssertionError("load " + i + ": " + image.getWidth(null) + "x" + image.getHeight(null));
            }
        }
        System.out.println("BDImageConsumer: 20 loads complete, none errored");
        System.exit(0);
    }
}
