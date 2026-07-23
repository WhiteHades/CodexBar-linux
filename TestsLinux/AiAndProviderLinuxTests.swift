import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import Testing
@testable import CodexBarCLI
@testable import CodexBarCore

struct AiAndProviderLinuxTests {
    @Test
    func `sums complete spend in the newest billing currency`() async throws {
        let now = Date(timeIntervalSince1970: 1_800_000_000)
        let transport = ProviderHTTPTransportHandler { request in
            #expect(request.url?.absoluteString == "https://api.aiand.com/logs?range=30days&limit=100")
            #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer key")
            #expect(request.timeoutInterval == 15)
            return try Self.response(request, Self.finalPage)
        }
        let usage = try await AiAndUsageFetcher.fetchUsage(" key ", transport: transport, now: now)
        let snapshot = usage.toUsageSnapshot()

        #expect(usage.last30DaysSpend?.amount == Decimal(string: "8.12344"))
        #expect(usage.last30DaysSpend?.currencyCode == "JPY")
        #expect(usage.isComplete)
        #expect(snapshot.primary == nil)
        #expect(snapshot.providerCost?.limit == 0)
        #expect(snapshot.providerCost?.period == "Last 30 days")
        #expect(snapshot.dataConfidence == .exact)
        #expect(snapshot.updatedAt == now)
    }

    @Test
    func `paginates with both encoded cursors`() async throws {
        let recorder = AiAndRequestRecorder()
        let transport = ProviderHTTPTransportHandler { request in
            await recorder.append(request)
            return try Self.response(
                request,
                request.url?.query?.contains("after=") == true ? Self.finalPage : Self.firstPage)
        }
        let usage = try await AiAndUsageFetcher.fetchUsage("key", transport: transport)
        let requests = await recorder.values

        #expect(requests.count == 2)
        #expect(requests[1].url?.absoluteString ==
            "https://api.aiand.com/logs?range=30days&limit=100" +
            "&after=2026-07-17%2010:24:30.094374%2B00&after_id=row-2")
        #expect(usage.last30DaysSpend?.amount == Decimal(string: "20.62344"))
        #expect(usage.isComplete)
    }

    @Test
    func `marks missing cursor and page cap partial`() async throws {
        let missingCursor = ProviderHTTPTransportHandler { request in
            try Self.response(request, #"{"data":[{"cost":"2.5","currency":"jpy"}],"has_more":true}"#)
        }
        let partial = try await AiAndUsageFetcher.fetchUsage("key", transport: missingCursor)
        #expect(!partial.isComplete)
        #expect(partial.toUsageSnapshot().providerCost?.period == "Last 30 days (partial)")
        #expect(partial.toUsageSnapshot().dataConfidence == .estimated)

        let recorder = AiAndRequestRecorder()
        let capped = ProviderHTTPTransportHandler { request in
            await recorder.append(request)
            return try Self.response(request, Self.firstPage)
        }
        let cappedUsage = try await AiAndUsageFetcher.fetchUsage("key", transport: capped)
        #expect(await recorder.values.count == AiAndUsageFetcher.maxPages)
        #expect(cappedUsage.last30DaysSpend?.amount == Decimal(string: "125"))
        #expect(!cappedUsage.isComplete)
    }

    @Test
    func `uses exact decimals and omits unpriced windows`() async throws {
        let exactBody = #"{"data":[{"cost":"0.1","currency":"jpy"},"#
            + #"{"cost":"0.1","currency":"JPY"},{"cost":"0.1","currency":"jpy"},"#
            + #"{"cost":"9.5","currency":"usd"}],"has_more":false}"#
        let exact = ProviderHTTPTransportHandler { request in
            try Self.response(request, exactBody)
        }
        let usage = try await AiAndUsageFetcher.fetchUsage("key", transport: exact)
        #expect(usage.last30DaysSpend?.amount == Decimal(string: "0.3"))
        #expect(usage.last30DaysSpend?.currencyCode == "JPY")

        let empty = ProviderHTTPTransportHandler { request in
            try Self.response(request, #"{"data":[{"cost":"4.2","currency":null}],"has_more":false}"#)
        }
        #expect(try await AiAndUsageFetcher.fetchUsage("key", transport: empty).last30DaysSpend == nil)
    }

    @Test
    func `maps status parse credential and cancellation errors`() async {
        await #expect(throws: AiAndUsageError.notConfigured) {
            _ = try await AiAndUsageFetcher.fetchUsage(" ")
        }
        for (status, expected) in [
            (401, AiAndUsageError.authenticationRejected),
            (402, .insufficientCredits),
            (429, .rateLimited),
            (500, .apiError(500)),
        ] {
            let transport = ProviderHTTPTransportHandler { request in
                try Self.response(request, "{}", status: status)
            }
            await #expect {
                _ = try await AiAndUsageFetcher.fetchUsage("key", transport: transport)
            } throws: { $0 as? AiAndUsageError == expected }
        }
        let malformed = ProviderHTTPTransportHandler { request in try Self.response(request, "{}") }
        await #expect {
            _ = try await AiAndUsageFetcher.fetchUsage("key", transport: malformed)
        } throws: { error in
            guard case AiAndUsageError.parseFailed = error else { return false }
            return true
        }
        let cancelling = ProviderHTTPTransportHandler { _ in throw CancellationError() }
        await #expect(throws: CancellationError.self) {
            _ = try await AiAndUsageFetcher.fetchUsage("key", transport: cancelling)
        }
    }

    @Test
    func `reads settings and registry aliases`() throws {
        #expect(AiAndSettingsReader.apiKey(environment: ["AIAND_API_KEY": " 'key' "]) == "key")
        #expect(AiAndSettingsReader.apiKey(environment: [:]) == nil)
        let descriptor = ProviderDescriptorRegistry.descriptor(for: .aiand)
        #expect(descriptor.metadata.displayName == "ai&")
        #expect(descriptor.cli.aliases == ["ai&", "ai-and"])
        #expect(try #require(ProviderSelection(argument: "ai-and")).asList == [.aiand])
    }

    private static let finalPage = #"{"data":[{"cost":"7.02344000","currency":"jpy"},"#
        + #"{"cost":"1.10000000","currency":"jpy"},{"cost":null,"currency":"jpy"}],"has_more":false}"#
    private static let firstPage = #"{"data":[{"cost":"12.00000000","currency":"jpy"},"#
        + #"{"cost":"0.50000000","currency":"jpy"}],"has_more":true,"#
        + #""next_after":"2026-07-17 10:24:30.094374+00","next_after_id":"row-2"}"#

    private static func response(
        _ request: URLRequest,
        _ body: String,
        status: Int = 200) throws -> (Data, URLResponse)
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

private actor AiAndRequestRecorder {
    private(set) var values: [URLRequest] = []

    func append(_ request: URLRequest) {
        self.values.append(request)
    }
}
