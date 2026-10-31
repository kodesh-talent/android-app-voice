package com.xeno.voicelatency;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;

import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Blocking TLS byte transport handed to the native probe core.
 *
 * <p>The native layer owns the HTTP/WebSocket protocol; this class is deliberately
 * a dumb byte pipe so all wire handling stays in C++. Method names and signatures
 * are fixed by {@code jni_bridge.cpp}.
 */
public final class TlsTransport {

    private final SSLSocket socket;
    private final InputStream input;
    private final OutputStream output;
    private volatile boolean closed;
    private volatile String lastError;

    private TlsTransport(SSLSocket socket) throws IOException {
        this.socket = socket;
        this.input = socket.getInputStream();
        this.output = socket.getOutputStream();
    }

    /**
     * Opens a TLS connection with hostname verification enabled.
     *
     * @param host             server name
     * @param port             server port
     * @param connectTimeoutMs TCP connect timeout
     * @param readTimeoutMs    per-read timeout; 0 disables it
     */
    public static TlsTransport connect(String host, int port, int connectTimeoutMs,
                                       int readTimeoutMs) throws IOException {
        Socket plain = new Socket();
        plain.setTcpNoDelay(true);
        plain.connect(new InetSocketAddress(host, port), connectTimeoutMs);
        plain.setSoTimeout(readTimeoutMs);

        SSLSocketFactory factory = (SSLSocketFactory) SSLSocketFactory.getDefault();
        SSLSocket tls = (SSLSocket) factory.createSocket(plain, host, port, true);
        tls.setUseClientMode(true);
        tls.setSoTimeout(readTimeoutMs);

        SSLParameters parameters = tls.getSSLParameters();
        parameters.setEndpointIdentificationAlgorithm("HTTPS");
        tls.setSSLParameters(parameters);
        tls.startHandshake();

        return new TlsTransport(tls);
    }

    /**
     * Called from native code: returns bytes read, 0 on a read timeout, or -1 on
     * EOF/error. A timeout is not a closed socket: the native realtime loop uses
     * it to poll cancellation and continue receiving.
     */
    public int read(byte[] buffer) {
        if (closed) {
            return -1;
        }
        try {
            return input.read(buffer, 0, buffer.length);
        } catch (SocketTimeoutException e) {
            return 0;
        } catch (IOException e) {
            lastError = e.getMessage();
            return -1;
        }
    }

    /** Called from native code: writes {@code length} bytes and flushes. */
    public void write(byte[] data, int length) {
        if (closed) {
            return;
        }
        try {
            output.write(data, 0, length);
            output.flush();
        } catch (IOException e) {
            lastError = e.getMessage();
            close();
        }
    }

    /** Called from native code when the exchange is finished. */
    public void close() {
        closed = true;
        try {
            socket.close();
        } catch (IOException ignored) {
            // Already closed.
        }
    }

    public boolean isClosed() {
        return closed;
    }

    public String lastError() {
        return lastError;
    }
}
