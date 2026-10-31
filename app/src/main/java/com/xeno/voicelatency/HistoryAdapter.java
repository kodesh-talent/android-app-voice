package com.xeno.voicelatency;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/** History list for completed probes. */
public final class HistoryAdapter extends RecyclerView.Adapter<HistoryAdapter.Holder> {

    private final List<ProbeResult> items = new ArrayList<>();
    private final SimpleDateFormat timeFormat = new SimpleDateFormat("HH:mm:ss", Locale.getDefault());

    public void add(ProbeResult result) {
        items.add(0, result);
        if (items.size() > 60) {
            items.remove(items.size() - 1);
        }
        notifyItemInserted(0);
    }

    public void clear() {
        items.clear();
        notifyDataSetChanged();
    }

    public List<ProbeResult> snapshot() {
        return new ArrayList<>(items);
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_history, parent, false);
        return new Holder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        ProbeResult result = items.get(position);
        String title = result.mode.title + " · " + ProbeResult.format(result.primaryMetricMs());
        holder.title.setText(title);

        String detail;
        if (result.ok) {
            detail = String.format(Locale.US, "首字节 %s · 总计 %s · %s",
                    ProbeResult.format(result.ttfbMs),
                    ProbeResult.format(result.totalMs),
                    timeFormat.format(new Date(result.wallClockMs)));
        } else {
            detail = result.diagnosis() + " · " + timeFormat.format(new Date(result.wallClockMs));
        }
        holder.subtitle.setText(detail);
        holder.badge.setText(result.ok ? "OK" : "失败");
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final TextView title;
        final TextView subtitle;
        final TextView badge;

        Holder(@NonNull View itemView) {
            super(itemView);
            title = itemView.findViewById(R.id.item_title);
            subtitle = itemView.findViewById(R.id.item_subtitle);
            badge = itemView.findViewById(R.id.item_badge);
        }
    }
}
