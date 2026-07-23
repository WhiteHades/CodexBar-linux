import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import Testing
@testable import CodexBarCLI
@testable import CodexBarCore

struct NeuralWattProviderLinuxTests {
    @Test
    func `maps subscription allowance and prepaid balance`() throws {
        let snapshot = try NeuralWattUsageFetcher._parseSnapshotForTesting(
            Data(Self.canonicalBody.utf8),
            updatedAt: Date(timeIntervalSince1970: 1))
        let usage = snapshot.toUsageSnapshot()

        #expect(usage.primary?.usedPercent == 13.9023 / 20 * 100)
        #expect(usage.primary?.resetDescription == "13.90 / 20 kWh")
        #expect(usage.primary?.windowMinutes == 43200)
        #expect(usage.primary?.resetsAt == snapshot.subscription?.currentPeriodEnd)
        #expect(usage.subscriptionRenewsAt == snapshot.subscription?.currentPeriodEnd)
        #expect(usage.extraRateWindows?.first?.id == "key-allowance")
        #expect(usage.extraRateWindows?.first?.title == "Key Monthly")
        #expect(usage.extraRateWindows?.first?.window.usedPercent == 25)
        #expect(usage.providerCost?.used == 32.6774)
        #expect(usage.providerCost?.limit == 0)
        #expect(usage.providerCost?.period == "Neuralwatt prepaid balance")
        #expect(usage.loginMethod(for: .neuralwatt) == "Standard plan")
        #expect(usage.dataConfidence == .exact)
    }

    @Test
    func `keeps zero balance separate and nonrenewing period`() throws {
        let body = #"{"balance":{"credits_remaining_usd":0,"total_credits_usd":0},"#
            + #""subscription":{"plan":"pro_energy","current_period_start":"2026-04-01T00:00:00.123Z","#
            + #""current_period_end":"2026-05-01T00:00:00.456Z","auto_renew":false,"#
            + #""kwh_included":10,"kwh_used":2.5,"kwh_remaining":7.5},"#
            + #""key":{"allowance":{"blocked":true,"period":"monthly"}}}"#
        let usage = try NeuralWattUsageFetcher._parseSnapshotForTesting(
            Data(body.utf8),
            updatedAt: Date(timeIntervalSince1970: 2))
            .toUsageSnapshot()

        #expect(usage.primary?.usedPercent == 25)
        #expect(usage.primary?.resetDescription == "2.50 / 10 kWh")
        #expect(usage.primary?.resetsAt != nil)
        #expect(usage.subscriptionRenewsAt == nil)
        #expect(usage.extraRateWindows?.first?.window.usedPercent == 100)
        #expect(usage.providerCost?.used == 0)
        #expect(usage.loginMethod(for: .neuralwatt) == "Pro Energy plan")
    }

    @Test
    func `builds endpoint and bearer request`() async throws {
        let transport = ProviderHTTPTransportHandler { request in
            #expect(request.url?.absoluteString == "https://proxy.test/v1/quota")
            #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer key")
            #expect(request.value(forHTTPHeaderField: "Accept") == "application/json")
            #expect(request.timeoutInterval == 15)
            return try Self.response(request, Self.minimalBody)
        }
        let usage = try await NeuralWattUsageFetcher.fetchUsage(
            apiKey: " key ",
            environment: [NeuralWattSettingsReader.apiURLEnvironmentKey: "proxy.test/v1"],
            transport: transport)

        #expect(usage.effectiveRemainingCredits == 5)
    }

    @Test
    func `retries transient response and preserves cancellation`() async throws {
        let sequence = NeuralWattLinuxSequenceTransport(statuses: [503, 200])
        let usage = try await NeuralWattUsageFetcher.fetchUsage(
            apiKey: "key",
            environment: [:],
            transport: sequence,
            retryPolicy: ProviderHTTPRetryPolicy(maxRetries: 1, baseDelaySeconds: 0, maxDelaySeconds: 0))
        #expect(usage.effectiveRemainingCredits == 5)
        #expect(await sequence.requestCount == 2)

        let cancelling = ProviderHTTPTransportHandler { _ in throw CancellationError() }
        await #expect(throws: CancellationError.self) {
            _ = try await NeuralWattUsageFetcher.fetchUsage(
                apiKey: "key",
                environment: [:],
                transport: cancelling)
        }
    }

    @Test
    func `rejects malformed responses credentials and endpoints`() async {
        await #expect {
            _ = try await NeuralWattUsageFetcher.fetchUsage(apiKey: " ", environment: [:])
        } throws: { error in
            guard case NeuralWattUsageError.missingCredentials = error else { return false }
            return true
        }
        await #expect(throws: NeuralWattSettingsError.invalidEndpointOverride(
            NeuralWattSettingsReader.apiURLEnvironmentKey))
        {
            _ = try await NeuralWattUsageFetcher.fetchUsage(
                apiKey: "key",
                environment: [NeuralWattSettingsReader.apiURLEnvironmentKey: "https://user@example.com"])
        }
        #expect(throws: NeuralWattUsageError.self) {
            _ = try NeuralWattUsageFetcher._parseSnapshotForTesting(
                Data(#"{"balance":{}}"#.utf8),
                updatedAt: .distantPast)
        }

        #expect(NeuralWattSettingsReader.apiKey(environment: ["NEURALWATT_API_KEY": " 'key' "]) == "key")
        let descriptor = ProviderDescriptorRegistry.descriptor(for: .neuralwatt)
        #expect(descriptor.cli.aliases == ["nw", "neural"])
        let selection = ProviderSelection(argument: "nw")
        #expect(selection?.asList == [.neuralwatt])
    }

    fileprivate static let minimalBody = #"{"balance":{"credits_remaining_usd":5},"#
        + #""subscription":null,"key":{"name":"retry","allowance":null}}"#
    private static let canonicalBody = #"{"snapshot_at":"2026-04-16T18:30:00Z","#
        + #""balance":{"credits_remaining_usd":32.6774,"total_credits_usd":52.34,"#
        + #""credits_used_usd":19.6626,"accounting_method":"energy"},"#
        + #""usage":{"lifetime":{"cost_usd":243.9145,"requests":37801,"tokens":1235477176,"#
        + #""energy_kwh":15.6009},"current_month":{"cost_usd":160.1463,"requests":23902,"#
        + #""tokens":1116658995,"energy_kwh":9.7278}},"#
        + #""limits":{"overage_limit_usd":null,"rate_limit_tier":"standard"},"#
        + #""subscription":{"plan":"standard","status":"active","billing_interval":"month","#
        + #""current_period_start":"2026-04-11T05:05:25Z","#
        + #""current_period_end":"2026-05-11T05:05:25Z","auto_renew":true,"#
        + #""kwh_included":20,"kwh_used":13.9023,"kwh_remaining":6.0977,"in_overage":false},"#
        + #""key":{"name":"production","allowance":{"limit_usd":50,"period":"monthly","#
        + #""spent_usd":12.5,"remaining_usd":37.5,"blocked":false}}}"#

    fileprivate static func response(_ request: URLRequest, _ body: String, status: Int = 200) throws
        -> (Data, URLResponse)
    {
        guard let url = request.url,
              let response = HTTPURLResponse(
                  url: url,
                  statusCode: status,
                  httpVersion: "HTTP/1.1",
                  headerFields: ["Content-Type": "application/json"])
        else { throw URLError(.badServerResponse) }
        return (Data(body.utf8), response)
    }
}

private actor NeuralWattLinuxSequenceTransport: ProviderHTTPTransport {
    private var statuses: [Int]
    private(set) var requestCount = 0

    init(statuses: [Int]) {
        self.statuses = statuses
    }

    func data(for request: URLRequest) async throws -> (Data, URLResponse) {
        self.requestCount += 1
        let status = self.statuses.isEmpty ? 200 : self.statuses.removeFirst()
        return try NeuralWattProviderLinuxTests.response(
            request,
            NeuralWattProviderLinuxTests.minimalBody,
            status: status)
    }
}
