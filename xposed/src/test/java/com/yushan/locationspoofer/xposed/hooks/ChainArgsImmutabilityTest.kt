package com.yushan.locationspoofer.xposed.hooks

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * `XposedInterface.Chain.getArgs()` 返回只读列表：`chain.args[i] = …` 或
 * `chain.args.add/set/remove/clear(...)` 会在每次回调抛 UnsupportedOperationException
 * （2026-10-16 在 Google Play 服务进程实测每秒一次、5 分钟 291 次）。异常由 JDK 的只读 List
 * 抛出，触发者是我们的非法赋值；框架只负责如何处理与记录，不能据日志认定它一定回抛给调用者。
 *
 * 换参的唯一途径是复制成数组后交给 `proceed(args)` / `proceedWith(this, args)`。
 * 本测试按源码契约守卫，并自测检测规则不误伤合法的读取与比较。
 */
class ChainArgsImmutabilityTest {
    private val mutations = listOf(
        // 赋值（含 += 等复合赋值），但排除 == 与 ===（读取/比较是合法的）
        Regex("""\.args\s*\[[^\]]*\]\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)"""),
        // 元素自增/自减同样会走 List.set
        Regex("""\.args\s*\[[^\]]*\]\s*(?:\+\+|--)"""),
        Regex("""\.args\s*\.\s*(set|add|remove|clear)\s*\("""),
    )

    private fun mutates(line: String) = mutations.any { it.containsMatchIn(line) }

    @Test
    fun detectorAllowsReadsAndComparisons() {
        listOf(
            "if (chain.args[0] == null) return",
            "val same = innerChain.args[0] === original",
            "if (chain.args[0] != null && chain.args.isNotEmpty()) {",
            "val args = chain.args.toTypedArray()",
            "args[0] = spoofedStatus", // 局部数组，合法
            "chain.args.forEach { XposedBridge.log(it.toString()) }",
            "val n = chain.args.size",
        ).forEach { assertFalse("合法读取被判为改写: $it", mutates(it)) }
    }

    @Test
    fun detectorRejectsMutations() {
        listOf(
            "chain.args[0] = true",
            "innerChain.args[0] = spoofedStatus",
            "chain.args[0] += 1",
            "chain.args[0] -= 1",
            "chain.args[0]++",
            "chain.args.set(0, value)",
            "chain.args.add(value)",
            "chain.args.remove(0)",
            "chain.args.clear()",
        ).forEach { assertTrue("非法改写未被识别: $it", mutates(it)) }
    }

    @Test
    fun hookSourcesNeverMutateTheChainArgsList() {
        val candidates = listOf(File("src/main/java"), File("xposed/src/main/java"))
        val root = candidates.firstOrNull { it.isDirectory }
        assertTrue("找不到源码目录，守卫失效: ${candidates.map { it.absolutePath }}", root != null)
        val offenders = mutableListOf<String>()
        root!!.walkTopDown()
            // 本文件自身含用于自测的样本字符串，按文件名排除
            .filter { it.isFile && it.extension in setOf("kt", "java") && it.name != "ChainArgsImmutabilityTest.kt" }
            .forEach { file ->
                file.readLines().forEachIndexed { index, line ->
                    if (mutates(line)) offenders += "${file.path}:${index + 1}: ${line.trim()}"
                }
            }
        assertTrue(
            "chain.args 是只读列表，禁止用 [] = / set / add / remove / clear 改写，请改为 " +
                "`val args = chain.args.toTypedArray(); args[0] = …; proceed(args)`:\n" +
                offenders.joinToString("\n"),
            offenders.isEmpty()
        )
    }
}
