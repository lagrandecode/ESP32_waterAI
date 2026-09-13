import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/material.dart';
import 'package:intl/intl.dart';

import 'firebase_options.dart';

// GitHub dark-mode contribution palette
const _bg = Color(0xFF0D1117);
const _card = Color(0xFF161B22);
const _border = Color(0xFF30363D);
const _textPrimary = Color(0xFFE6EDF3);
const _textMuted = Color(0xFF8D96A0);
const _cellEmpty = Color(0xFF1B2129);
const _greens = [Color(0xFF0E4429), Color(0xFF006D32), Color(0xFF26A641), Color(0xFF39D353)];
const _accent = Color(0xFF39D353);

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(options: DefaultFirebaseOptions.currentPlatform);
  await FirebaseAuth.instance.signInAnonymously();
  runApp(const WaterAiApp());
}

class WaterAiApp extends StatelessWidget {
  const WaterAiApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Water AI',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: _bg,
        fontFamily: 'SF Pro Text',
      ),
      home: const HistoryScreen(),
    );
  }
}

class HistoryScreen extends StatelessWidget {
  const HistoryScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final query =
        FirebaseFirestore.instance.collection('water_events').orderBy('ts').snapshots();

    return Scaffold(
      appBar: AppBar(
        backgroundColor: _bg,
        elevation: 0,
        title: const Row(
          children: [
            Icon(Icons.water_drop, color: _accent, size: 22),
            SizedBox(width: 8),
            Text('Water AI',
                style: TextStyle(color: _textPrimary, fontWeight: FontWeight.w700, fontSize: 20)),
          ],
        ),
      ),
      body: StreamBuilder<QuerySnapshot>(
        stream: query,
        builder: (context, snapshot) {
          if (snapshot.hasError) {
            return Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Text('Could not load data:\n${snapshot.error}',
                    textAlign: TextAlign.center, style: const TextStyle(color: _textMuted)),
              ),
            );
          }
          if (!snapshot.hasData) {
            return const Center(child: CircularProgressIndicator(color: _accent));
          }

          // Aggregate events into per-day counts (local time)
          final counts = <DateTime, int>{};
          for (final doc in snapshot.data!.docs) {
            final ts = (doc['ts'] as Timestamp).toDate().toLocal();
            final day = DateTime(ts.year, ts.month, ts.day);
            counts[day] = (counts[day] ?? 0) + 1;
          }

          final now = DateTime.now();
          final today = DateTime(now.year, now.month, now.day);
          final todayCount = counts[today] ?? 0;
          final total = counts.values.fold<int>(0, (a, b) => a + b);

          int streak = 0;
          var cursor = today;
          while ((counts[cursor] ?? 0) > 0) {
            streak++;
            cursor = cursor.subtract(const Duration(days: 1));
          }

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              Row(
                children: [
                  Expanded(child: _StatCard(label: 'Today', value: '$todayCount', unit: 'glasses')),
                  const SizedBox(width: 12),
                  Expanded(child: _StatCard(label: 'Streak', value: '$streak', unit: 'days')),
                  const SizedBox(width: 12),
                  Expanded(child: _StatCard(label: 'Total', value: '$total', unit: 'glasses')),
                ],
              ),
              const SizedBox(height: 20),
              _Panel(
                title: '$total glasses in the last year',
                child: Heatmap(counts: counts),
              ),
              const SizedBox(height: 20),
              _Panel(
                title: 'Recent',
                child: Column(
                  children: snapshot.data!.docs.reversed
                      .take(10)
                      .map((doc) {
                        final ts = (doc['ts'] as Timestamp).toDate().toLocal();
                        return Padding(
                          padding: const EdgeInsets.symmetric(vertical: 6),
                          child: Row(
                            children: [
                              const Icon(Icons.water_drop_outlined, color: _accent, size: 16),
                              const SizedBox(width: 10),
                              Text(DateFormat('EEE, MMM d').format(ts),
                                  style: const TextStyle(color: _textPrimary, fontSize: 14)),
                              const Spacer(),
                              Text(DateFormat('h:mm a').format(ts),
                                  style: const TextStyle(color: _textMuted, fontSize: 13)),
                            ],
                          ),
                        );
                      })
                      .toList(),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

class _StatCard extends StatelessWidget {
  const _StatCard({required this.label, required this.value, required this.unit});
  final String label;
  final String value;
  final String unit;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 12),
      decoration: BoxDecoration(
        color: _card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: _border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(color: _textMuted, fontSize: 12)),
          const SizedBox(height: 6),
          Text(value,
              style: const TextStyle(
                  color: _textPrimary, fontSize: 26, fontWeight: FontWeight.w700)),
          Text(unit, style: const TextStyle(color: _textMuted, fontSize: 11)),
        ],
      ),
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel({required this.title, required this.child});
  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: _card,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: _border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: const TextStyle(color: _textPrimary, fontSize: 14)),
          const SizedBox(height: 12),
          child,
        ],
      ),
    );
  }
}

/// GitHub-style contribution heatmap: one column per week, one row per weekday.
class Heatmap extends StatelessWidget {
  const Heatmap({super.key, required this.counts, this.weeks = 52});
  final Map<DateTime, int> counts;
  final int weeks;

  Color _cellColor(int count) {
    if (count <= 0) return _cellEmpty;
    if (count == 1) return _greens[0];
    if (count == 2) return _greens[1];
    if (count <= 4) return _greens[2];
    return _greens[3];
  }

  @override
  Widget build(BuildContext context) {
    final now = DateTime.now();
    final today = DateTime(now.year, now.month, now.day);
    // End the grid on the Saturday of the current week (GitHub weeks run Sun-Sat)
    final gridEnd = today.add(Duration(days: 6 - today.weekday % 7));

    const cell = 13.0;
    const gap = 3.0;

    final columns = <Widget>[];
    String? lastMonthLabel;
    final monthLabels = <Widget>[];

    for (int w = weeks - 1; w >= 0; w--) {
      final weekStart = gridEnd.subtract(Duration(days: w * 7 + 6));

      // Month label above the column where a new month starts
      final label = DateFormat('MMM').format(weekStart);
      final showLabel = label != lastMonthLabel && weekStart.day <= 7;
      if (showLabel) lastMonthLabel = label;
      monthLabels.add(SizedBox(
        width: cell + gap,
        child: showLabel
            ? Text(label,
                style: const TextStyle(color: _textMuted, fontSize: 9),
                overflow: TextOverflow.visible,
                softWrap: false)
            : null,
      ));

      columns.add(Column(
        children: List.generate(7, (d) {
          final day = weekStart.add(Duration(days: d));
          final isFuture = day.isAfter(today);
          return Container(
            width: cell,
            height: cell,
            margin: const EdgeInsets.only(bottom: gap, right: gap),
            decoration: BoxDecoration(
              color: isFuture ? Colors.transparent : _cellColor(counts[day] ?? 0),
              borderRadius: BorderRadius.circular(3),
            ),
          );
        }),
      ));
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          reverse: true, // latest weeks visible first
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(children: monthLabels),
              const SizedBox(height: 4),
              Row(crossAxisAlignment: CrossAxisAlignment.start, children: columns),
            ],
          ),
        ),
        const SizedBox(height: 8),
        Row(
          mainAxisAlignment: MainAxisAlignment.end,
          children: [
            const Text('Less', style: TextStyle(color: _textMuted, fontSize: 11)),
            const SizedBox(width: 6),
            ...[_cellEmpty, ..._greens].map((c) => Container(
                  width: 11,
                  height: 11,
                  margin: const EdgeInsets.symmetric(horizontal: 2),
                  decoration:
                      BoxDecoration(color: c, borderRadius: BorderRadius.circular(3)),
                )),
            const SizedBox(width: 6),
            const Text('More', style: TextStyle(color: _textMuted, fontSize: 11)),
          ],
        ),
      ],
    );
  }
}
